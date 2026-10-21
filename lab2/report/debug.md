# 分支任务：GDB 调试 QEMU 的页表查询与地址翻译

## 1. 任务目标

本分支任务要求：

1. 理解 QEMU 模拟 RISC-V Sv39 地址翻译的关键调用路径和分支语句；
2. 使用两个 GDB 会话，同时调试 **QEMU 模拟器源码** 和 **运行在 QEMU 中的 ucore 内核**；
3. 单步观察一条访存指令的虚拟地址如何被 QEMU 翻译成物理地址；
4. 找到 QEMU 4.1.1 中模拟 TLB 查找的代码，说明 TLB 命中/未命中的处理；
5. 比较 QEMU 模拟 TLB 与真实 CPU TLB 的逻辑差异；
6. 记录调试过程中的问题、AI 协作过程和有趣细节。

本文以一次真实的 `memset(edata, 0, end - edata)` 存储访问为例：

```text
虚拟地址：0xffffffffc0207018
物理地址：0x80207018
```

---

## 2. 调试环境

### 2.1 为什么需要调试版 QEMU

系统里通过包管理器安装的 QEMU 通常是 stripped 的，只有可执行文件，没有调试符号和源码映射。GDB 无法在 QEMU 源码中设置断点，也无法查看 `get_physical_address` 的局部变量。

因此需要编译一个带调试信息的 QEMU：

```bash
cd ~/qemu-debug
wget https://download.qemu.org/qemu-4.1.1.tar.xz
tar xJf qemu-4.1.1.tar.xz
cd qemu-4.1.1

./configure --target-list=riscv64-softmmu \
            --enable-debug \
            --disable-werror \
            --python=python3

make -j$(nproc)
```

编译完成后得到：

```text
~/qemu-debug/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64
```

**注意：不要执行 `sudo make install`。** 这样系统里同时保留普通 QEMU 和调试版 QEMU，调试版只用于本分支任务。

### 2.2 编译 ucore

```bash
cd '/mnt/c/Users/21433/Desktop/操作系统/lab2/lab2_new'
make clean
make
```

确认内核镜像：

```bash
ls -l bin/ucore.img bin/kernel
```

### 2.3 获取本次调试的虚拟地址

为了让条件断点只在我们关心的访存处停下，需要选一个确定会被访问的地址。本实验选择 `edata`：

```bash
riscv64-unknown-elf-nm -n bin/kernel | grep ' edata$'
```

本次构建得到：

```text
ffffffffc0207018 D edata
```

因此本次调试的虚拟地址是：

```text
0xffffffffc0207018
```

`edata` 是数据段结束地址，`kern_init()` 的第一条语句 `memset(edata, 0, end - edata)` 会向该地址写入 0，所以它一定会被访问。

---

## 3. 三个终端的调试流程

### 终端 1：启动调试版 QEMU

```bash
cd '/mnt/c/Users/21433/Desktop/操作系统/lab2/lab2_new'

/root/qemu-debug/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64 \
  -machine virt \
  -nographic \
  -bios default \
  -kernel bin/ucore.img \
  -s -S
```

- `-s`：在 `localhost:1234` 打开 GDB stub；
- `-S`：QEMU 暂停在初始状态，等待 GDB 连接。

### 终端 2：附加到 QEMU 进程（调试 QEMU 源码）

先找到 QEMU 的 PID：

```bash
pgrep -f qemu-system-riscv64
```

然后启动宿主机的 x86 GDB：

```bash
gdb
```

在 GDB 中：

```gdb
attach <QEMU_PID>
handle SIGPIPE nostop noprint

# 在 QEMU 的 RISC-V 地址翻译函数中设置条件断点
break target/riscv/cpu_helper.c:238 if addr == 0xffffffffc0207018
continue
```

`handle SIGPIPE nostop noprint` 用于避免 QEMU 与 GDB 通信时产生大量无意义的 SIGPIPE 中断。

### 终端 3：连接 QEMU 的 GDB stub（调试 ucore）

```bash
cd '/mnt/c/Users/21433/Desktop/操作系统/lab2/lab2_new'
riscv64-unknown-elf-gdb bin/kernel
```

在 GDB 中：

```gdb
set arch riscv:rv64
target remote localhost:1234
hbreak kern_init
continue
```

这里使用 `hbreak`（硬件断点）而不是普通 `break`，因为 `kern_init` 位于高地址虚拟空间，打开分页后硬件断点更可靠。

当终端 3 停在 `kern_init` 时，说明 ucore 已经进入 C 语言入口；继续执行：

```gdb
continue
```

此时终端 2 的 QEMU 源码断点会命中，因为 `memset` 正在向 `edata` 写入。

---

## 4. QEMU 中的关键调用路径

这次存储访问的完整路径可以概括为：

```text
ucore 执行 sb 指令
  -> TCG 生成代码调用 helper_ret_stb_mmu()
  -> store_helper()                         [accel/tcg/cputlb.c]
       -> tlb_hit(tlb_addr, addr)           [include/exec/cpu-all.h]
       -> TLB miss:
            tlb_fill()                      [accel/tcg/cputlb.c]
            -> riscv_cpu_tlb_fill()         [target/riscv/cpu_helper.c]
            -> get_physical_address()       [target/riscv/cpu_helper.c]
                 -> 逐级读取 PTE
                 -> 计算 physical
            -> tlb_set_page()               [accel/tcg/cputlb.c]
       -> TLB hit:
            由 TCG 生成代码直接使用 TLB 的 addend
            haddr = addr + entry->addend
            stb_p(haddr, val)
```

### 4.1 TLB 命中/未命中的入口

`accel/tcg/cputlb.c` 的 `store_helper()`：

```c
static inline void store_helper(...)
{
    uintptr_t index = tlb_index(env, mmu_idx, addr);
    CPUTLBEntry *entry = tlb_entry(env, mmu_idx, addr);
    target_ulong tlb_addr = tlb_addr_write(entry);

    /* If the TLB entry is for a different page, reload and try again. */
    if (!tlb_hit(tlb_addr, addr)) {
        if (!victim_tlb_hit(env, mmu_idx, index, tlb_off,
                            addr & TARGET_PAGE_MASK)) {
            tlb_fill(env_cpu(env), addr, size, MMU_DATA_STORE,
                     mmu_idx, retaddr);
            ...
        }
    }
    ...
}
```

`tlb_hit()` 和 `tlb_hit_page()` 在 `include/exec/cpu-all.h`：

```c
static inline bool tlb_hit_page(target_ulong tlb_addr, target_ulong addr)
{
    return addr == (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK));
}

static inline bool tlb_hit(target_ulong tlb_addr, target_ulong addr)
{
    return tlb_hit_page(tlb_addr, addr & TARGET_PAGE_MASK);
}
```

也就是说，QEMU 的 TLB entry 中保存的是 **虚拟页号 tag**，命中判断就是把访问地址按页对齐后与 tag 比较。

### 4.2 TLB 未命中：进入 RISC-V 页表遍历

`tlb_fill()` 调用 CPU 类的 `tlb_fill` 回调，在 RISC-V 上就是 `riscv_cpu_tlb_fill()`：

```c
ret = get_physical_address(env, &pa, &prot, address, access_type, mmu_idx);
...
if (ret == TRANSLATE_SUCCESS) {
    tlb_set_page(cs, address & TARGET_PAGE_MASK, pa & TARGET_PAGE_MASK,
                 prot, mmu_idx, TARGET_PAGE_SIZE);
    return true;
}
```

`get_physical_address()` 是 QEMU 模拟 RISC-V MMU 页表遍历的核心函数。

---

## 5. 真实 GDB 调试记录

### 5.1 第一次存储：TLB miss

在 `store_helper()` 的 `if (!tlb_hit(tlb_addr, addr))` 处停下：

```text
Breakpoint 1.1, store_helper (... addr=18446744072637935640 ...)
1519    if (!tlb_hit(tlb_addr, addr)) {
```

`18446744072637935640` 的十六进制就是：

```text
0xffffffffc0207018
```

打印 TLB 项：

```gdb
p/x addr
p/x tlb_addr
p/x entry->addend
```

输出：

```text
$1 = 0xffffffffc0207018
$2 = 0xffffffffffffffff
$3 = 0xffffffffffffffff
```

`tlb_addr = 0xffffffffffffffff` 表示该 TLB 项无效（所有位为 1），所以 `tlb_hit()` 为假，进入 `tlb_fill()` 的慢路径。

### 5.2 进入 `get_physical_address()`

在 `target/riscv/cpu_helper.c:238` 停下：

```text
Breakpoint 2, get_physical_address (... addr=18446744072637935640, access_type=1, mmu_idx=1)
238    target_ulong idx = (addr >> (PGSHIFT + ptshift)) &
```

打印参数和局部变量：

```gdb
p/x addr
p access_type
p mmu_idx
p/x env->satp
p levels
p ptidxbits
p ptesize
p/x base
p ptshift
p i
```

输出：

```text
$4  = 0xffffffffc0207018
$5  = 1                    # MMU_DATA_STORE
$6  = 1                    # mmu_idx = 1
$7  = 0x8000000000080206   # Sv39, root PPN = 0x80206
$8  = 3                    # Sv39 有 3 级
$9  = 9                    # 每级 9 位索引
$10 = 8                    # PTE 8 字节
$11 = 0x80206000           # 根页表物理地址
$12 = 18                   # 第一级页内偏移位数
$13 = 0                    # i = 0，第一级
```

解释：

- `satp = 0x8000000000080206`：最高 4 位 `8` 表示 Sv39，低 44 位 `0x80206` 是根页表物理页号；
- 根页表物理地址 = `0x80206 << 12 = 0x80206000`；
- `levels = 3`、`ptidxbits = 9`，说明虚拟地址被分为 `VPN2[8:0]`、`VPN1[8:0]`、`VPN0[8:0]`、页内偏移 `[11:0]`。

### 5.3 计算页表项地址

在 `target/riscv/cpu_helper.c:242` 停下：

```text
242    target_ulong pte_addr = base + idx * ptesize;
```

打印：

```gdb
p i
p ptshift
p/x idx
continue
p/x pte_addr
```

输出：

```text
$14 = 0
$15 = 18
$16 = 0x1ff
$17 = 0x80206ff8
```

计算过程：

```text
idx = (0xffffffffc0207018 >> (12 + 18)) & 0x1ff
    = (0xffffffffc0207018 >> 30) & 0x1ff
    = 0x1ff

pte_addr = 0x80206000 + 0x1ff * 8
         = 0x80206ff8
```

这里的 `idx = 0x1ff = 511` 是根页表的最后一个页表项，对应虚拟地址最高的 1 GiB 区域。

### 5.4 读取 PTE

在 `target/riscv/cpu_helper.c:252` 停下：

```text
252    target_ulong pte = ldq_phys(cs->as, pte_addr);
```

继续后得到：

```text
$18 = 0x200000cf
$19 = 0x80000
```

解释：

- `pte = 0x200000cf`
- `PPN = pte >> 10 = 0x80000`
- 标志位 `0xcf = 0b11001111`：
  - `V = 1`：有效
  - `R = 1`：可读
  - `W = 1`：可写
  - `X = 1`：可执行
  - `A = 1`：已访问
  - `D = 1`：已修改

由于 `R/W/X` 不全是 0，这是一个 **叶子页表项**。又因为 `ptshift = 18`，它映射的是一个 **1 GiB 大页**，而不是 4 KiB 页。

### 5.5 计算物理地址

在 `target/riscv/cpu_helper.c:337` 和 `349` 处继续：

```text
$20 = 0x80207000
$21 = 0x7
```

解释：

- `*physical = 0x80207000`；
- `*prot = 0x7 = PAGE_READ | PAGE_WRITE | PAGE_EXEC`；
- 页内偏移 `addr & 0xfff = 0x018`；
- 最终物理地址：

```text
0x80207000 + 0x018 = 0x80207018
```

因此：

```text
VA 0xffffffffc0207018  ->  PA 0x80207018
```

### 5.6 页表遍历循环的解释

QEMU 的循环是：

```c
for (i = 0; i < levels; i++, ptshift -= ptidxbits) {
    target_ulong idx = (addr >> (PGSHIFT + ptshift)) &
                       ((1 << ptidxbits) - 1);
    target_ulong pte_addr = base + idx * ptesize;
    target_ulong pte = ldq_phys(cs->as, pte_addr);
    target_ulong ppn = pte >> PTE_PPN_SHIFT;

    if (!(pte & PTE_V)) {
        return TRANSLATE_FAIL;
    } else if (!(pte & (PTE_R | PTE_W | PTE_X))) {
        base = ppn << PGSHIFT;     /* 非叶子项，继续下一级 */
    } else {
        /* 叶子项，计算最终物理地址并返回 */
    }
}
```

它相当于硬件 MMU 的页表遍历：

1. 用 `VPN2` 查根页表；
2. 如果根 PTE 是内部节点（`R/W/X` 全 0），用其 PPN 作为下一级页表基址；
3. 用 `VPN1` 查二级页表；
4. 如果仍是内部节点，继续用 `VPN0` 查三级页表；
5. 找到叶子 PTE 后，把 PPN 与页内偏移组合成物理地址。

**本实验的重要发现**：本实验 lab2 的启动页表在根页表第 511 项直接放了一个 1 GiB 大页叶子，因此虽然 `levels = 3`，循环在 `i = 0` 就返回了，实际只执行了一轮。若使用 4 KiB 页表，则会执行三轮，分别使用 `VPN2`、`VPN1`、`VPN0`。这解释了为什么代码看起来是“三个循环/三次遍历”，而本次调试只看到一次。

---

## 6. QEMU 的 TLB 查找代码

### 6.1 TLB 命中判断

QEMU 的 TLB 查找入口在 `accel/tcg/cputlb.c` 的 `store_helper()`/`load_helper()`，而不是一个独立的 `tlb_lookup()` 函数。核心是：

```c
target_ulong tlb_addr = tlb_addr_write(entry);
if (!tlb_hit(tlb_addr, addr)) {
    if (!victim_tlb_hit(...)) {
        tlb_fill(...);
    }
}
```

其中：

- `tlb_index()`：根据虚拟地址和 MMU index 计算 TLB 表索引；
- `tlb_entry()`：取出 `CPUTLBEntry`；
- `tlb_addr_write()`：取写权限对应的 tag；
- `tlb_hit()`：比较虚拟页号；
- `victim_tlb_hit()`：检查 victim TLB；
- `tlb_fill()`：未命中时填充 TLB。

### 6.2 TLB 填充

`riscv_cpu_tlb_fill()` 在页表遍历成功后调用：

```c
tlb_set_page(cs, address & TARGET_PAGE_MASK, pa & TARGET_PAGE_MASK,
             prot, mmu_idx, TARGET_PAGE_SIZE);
```

`tlb_set_page_with_attrs()` 在 `accel/tcg/cputlb.c` 中：

```c
vaddr_page = vaddr & TARGET_PAGE_MASK;
paddr_page = paddr & TARGET_PAGE_MASK;

section = address_space_translate_for_iotlb(...);
...
if (!memory_region_is_ram(section->mr) && ...) {
    address |= TLB_MMIO;
    addend = 0;
} else {
    addend = (uintptr_t)memory_region_get_ram_ptr(section->mr) + xlat;
}
...
te = tlb_entry(env, mmu_idx, vaddr_page);
```

QEMU 的 TLB entry 不只是保存物理页号，还保存：

- 虚拟页号 tag；
- 对应的 host 内存指针偏移 `addend`；
- 读/写/取指权限；
- MMIO、ROM、notdirty 等标志；
- 用于快速访问的 host 地址信息。

命中后，TCG 生成代码可以直接计算：

```c
haddr = (void *)((uintptr_t)addr + entry->addend);
stb_p(haddr, val);
```

这就是 QEMU 软 MMU 的“TLB 命中快速路径”。

### 6.3 为什么第二次访问没有停在 C 代码断点

第一次 `memset` 的存储是 TLB miss，会进入 C 的 `store_helper()` 和页表遍历。

但 TLB 填充后，后续访问同一页时，QEMU 的 TCG 生成代码会走 **inline fast path**，直接在生成的代码里完成 `tlb_hit` 和 host 地址计算，不一定再次调用 C 的 `store_helper()`。所以：

- 第一次访问：C 断点命中，能看到完整的 `tlb_fill -> get_physical_address`；
- 后续访问：可能直接走 TCG 快速路径，不会再次命中 C 的 `store_helper` 断点。

这不是断点失效，而是 QEMU 把 TLB 命中路径优化到了生成代码中。这个细节很好地说明了“模拟器中的 TLB”既有软件数据结构，也有 JIT 生成代码的快速路径。

---

## 7. QEMU TLB 与真实 CPU TLB 的差异

| 维度 | QEMU 模拟 TLB | 真实 CPU TLB |
|------|---------------|--------------|
| 实现方式 | 软件数据结构 + TCG 生成代码 | 硬件 CAM/SRAM |
| 保存内容 | 虚拟页 tag、host 地址 addend、权限、MMIO/ROM/notdirty 标志 | VPN→PFN、权限、ASID/VMID、页大小等 |
| 填充者 | QEMU 的 `tlb_fill` / `tlb_set_page` | CPU 硬件页表遍历器 |
| 命中判断 | `tlb_hit()` 比较虚拟页号 | 硬件并行比较 tag |
| 失效 | `tlb_flush()`，通常由 `sfence.vma` 触发 | `sfence.vma` 使硬件 TLB 失效 |
| 可见性 | 调试器可以查看 QEMU 的 C 结构和变量 | OS 不能直接读取 TLB 内容 |
| 地址含义 | `addend` 直接指向 host 内存，便于 JIT 访问 | 只保存 guest 物理页号 |
| 特殊标志 | MMIO、ROM、notdirty、recheck 等模拟器标志 | 通常没有这些软件语义 |
| 性能目的 | 减少 QEMU 地址翻译和内存访问开销 | 减少 CPU 访问内存的延迟 |

逻辑上，QEMU TLB 模拟了“缓存虚拟页到物理页映射”的思想，但它还承担了“把 guest 地址映射到 host 地址”的职责，因此 entry 中会出现 `addend` 这种真实硬件 TLB 不会有的字段。

真实 CPU TLB 对 OS 是透明的，OS 只通过页表、`satp`、`sfence.vma` 等接口间接管理；QEMU TLB 则是 QEMU 内部实现，调试器可以直接观察其数据结构。

---

## 8. 关键分支语句

### 8.1 TLB 查找分支

`accel/tcg/cputlb.c`：

```c
if (!tlb_hit(tlb_addr, addr)) {          // TLB miss
    if (!victim_tlb_hit(...)) {          // victim TLB miss
        tlb_fill(...);                   // 进入页表遍历
    }
}

if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {  // IO 访问
    ...
}

if (size > 1 && ... ) {                  // 跨页/非对齐访问
    ...
}
```

### 8.2 页表项合法性分支

`target/riscv/cpu_helper.c`：

```c
if (!(pte & PTE_V)) {                              // 无效 PTE
    return TRANSLATE_FAIL;
} else if (!(pte & (PTE_R | PTE_W | PTE_X))) {     // 内部节点
    base = ppn << PGSHIFT;                         // 继续下一级
} else if ((pte & (PTE_R | PTE_W | PTE_X)) == PTE_W) {
    return TRANSLATE_FAIL;                         // 保留的 W-only 组合
} else if ((pte & (PTE_R | PTE_W | PTE_X)) == (PTE_W | PTE_X)) {
    return TRANSLATE_FAIL;                         // 保留的 W+X 组合
} else if ((pte & PTE_U) && ...) {                 // 用户/权限检查
    return TRANSLATE_FAIL;
} else if (ppn & ((1ULL << ptshift) - 1)) {        // 大页 PPN 未对齐
    return TRANSLATE_FAIL;
} else if (access_type == MMU_DATA_LOAD && ...) {  // 读权限
    return TRANSLATE_FAIL;
} else if (access_type == MMU_DATA_STORE && !(pte & PTE_W)) {  // 写权限
    return TRANSLATE_FAIL;
} else if (access_type == MMU_INST_FETCH && !(pte & PTE_X)) {  // 执行权限
    return TRANSLATE_FAIL;
} else {
    /* 叶子项：更新 A/D 位，计算物理地址 */
}
```

### 8.3 A/D 位更新分支

```c
target_ulong updated_pte = pte | PTE_A |
    (access_type == MMU_DATA_STORE ? PTE_D : 0);

if (updated_pte != pte) {
    MemoryRegion *mr;
    mr = address_space_translate(cs->as, pte_addr, ...);
    if (memory_region_is_ram(mr)) {
        /* 原子更新 PTE 的 A/D 位 */
    } else {
        return TRANSLATE_FAIL;
    }
}
```

本实验的 PTE 已经是 `0x200000cf`，A/D 位已经置 1，所以不会进入这个更新分支。

---

## 9. 调试要求逐条回答

### 9.1 关键调用路径和分支语句

调用路径：

```text
sb
 -> helper_ret_stb_mmu()
 -> store_helper()
 -> tlb_hit() / victim_tlb_hit()
 -> tlb_fill()
 -> riscv_cpu_tlb_fill()
 -> get_physical_address()
 -> ldq_phys() 读取 PTE
 -> 叶子 PTE -> 计算 physical
 -> tlb_set_page()
 -> stb_p() 写 host 内存
```

关键分支见第 8 节。

### 9.2 单步调试页表翻译

本次调试的关键数值：

```text
VA       = 0xffffffffc0207018
satp     = 0x8000000000080206
root     = 0x80206000
levels   = 3
VPN2     = 0x1ff
PTE addr = 0x80206ff8
PTE      = 0x200000cf
PPN      = 0x80000
physical = 0x80207000
offset   = 0x018
PA       = 0x80207018
```

对应流程：

1. 用 `VPN2=0x1ff` 查根页表，得到 `PTE=0x200000cf`；
2. PTE 的 `R/W/X` 非 0，是叶子项；
3. PPN=0x80000，`ptshift=18`，说明是 1 GiB 大页；
4. 把 PPN 与 VPN 低位组合，得到 `0x80207000`；
5. 加上页内偏移 `0x018`，得到 `0x80207018`。

### 9.3 QEMU 4.1.1 中查找 TLB 的 C 代码

可以找到，主要在：

- `accel/tcg/cputlb.c`：`store_helper()`、`load_helper()`、`tlb_fill()`、`tlb_set_page()`；
- `include/exec/cpu-all.h`：`tlb_hit_page()`、`tlb_hit()`；
- `target/riscv/cpu_helper.c`：`riscv_cpu_tlb_fill()`、`get_physical_address()`。

TLB miss 时走 C 慢路径；TLB hit 时可能直接走 TCG 生成的快速路径。

### 9.4 QEMU TLB 与真实 CPU TLB 的逻辑区别

见第 7 节。最关键的区别是：

- 真实 TLB 只缓存 guest 虚拟页到物理页的映射；
- QEMU TLB 还缓存 host 地址 `addend`、MMIO 标志等模拟器专用信息；
- 真实 TLB 是硬件并行查找；QEMU TLB 是软件结构，由 TCG 生成的代码快速访问；
- OS 不能直接看到真实 TLB，但调试器可以看到 QEMU 的 TLB 结构。

### 9.5 有趣细节

1. **`satp` 的值**：`0x8000000000080206` 中最高位模式是 `8`，正好对应 Sv39；PPN 是 `0x80206`。
2. **1 GiB 大页**：虽然 Sv39 有三级页表，本实验的根 PTE 直接是叶子，所以只遍历了一级。这一点和“三个循环”的直觉不同。
3. **PTE 的 A/D 位**：`0xcf` 已经包含 A/D，因此没有触发 QEMU 的原子更新分支。
4. **host addend**：QEMU TLB 命中后直接使用 `addr + addend` 访问 host 内存，这是模拟器特有的优化。
5. **TCG 快速路径**：TLB 命中不一定再次进入 C 的 `store_helper`，说明 QEMU 的 TLB 查找部分在 JIT 生成代码中完成。
6. **双 GDB**：宿主机 GDB 调试 QEMU 进程，RISC-V GDB 通过 QEMU GDB stub 调试 guest 内核；两个 GDB 的目标和架构完全不同。

### 9.6 AI 协作记录

| 问题 | AI 给出的思路 | 实际处理 |
|------|---------------|----------|
| 系统 QEMU 没有调试符号 | 从源码编译 QEMU 4.1.1，使用 `--enable-debug`，不要 `make install` | 编译出 `riscv64-softmmu/qemu-system-riscv64`，用绝对路径启动 |
| 如何同时调试 QEMU 和 ucore | 两个 GDB：宿主 x86 GDB attach QEMU 进程；RISC-V GDB 连接 QEMU GDB stub | 使用三个终端完成双 GDB |
| attach 后出现 SIGPIPE 中断 | 在宿主 GDB 中执行 `handle SIGPIPE nostop noprint` | 加入 GDB 脚本，避免通信信号干扰 |
| 如何让断点只停在目标访存 | 选择确定会访问的地址，使用条件断点 `addr == ...` | 使用 `edata=0xffffffffc0207018` |
| 为什么只遍历一级页表 | 检查 PTE 的 `R/W/X` 和 `ptshift`，判断是否为超级页 | 发现根 PTE 是 1 GiB 叶子，解释清楚 |
| 为什么第二次存储没有停在 C 断点 | TLB 命中走 TCG 生成代码的快速路径 | 通过源码确认 `tlb_hit`/`addend` 快速路径 |
| 如何确认物理地址 | 打印 `*physical`，再加页内偏移 | 得到 `0x80207000 + 0x018 = 0x80207018` |

---

## 10. 复现脚本

仓库中提供了：

```text
lab2/debug/dual_debug.sh
```

运行方式：

```bash
cd OS2026-Labs/lab2
QEMU_DEBUG=/root/qemu-debug/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64 \
  bash debug/dual_debug.sh
```

脚本会自动：

1. 编译 ucore；
2. 从 `bin/kernel` 读取 `edata` 地址；
3. 启动调试版 QEMU；
4. 在宿主 GDB 中设置 QEMU 源码条件断点；
5. 在 RISC-V GDB 中停在 `kern_init`；
6. 打印 TLB miss、页表遍历和物理地址计算过程。

---

## 11. 结论

本次调试完整观察了：

```text
ucore 的存储指令
  -> QEMU TLB 查找
  -> TLB miss
  -> RISC-V 页表遍历
  -> 1 GiB 叶子 PTE
  -> 计算物理地址
  -> TLB 填充
  -> host 内存写入
```

通过双 GDB 调试，我们不仅验证了 `VA 0xffffffffc0207018 -> PA 0x80207018` 的翻译结果，还理解了 QEMU 软 MMU 与真实 CPU MMU/TLB 的联系和差异：QEMU 用软件数据结构和 TCG 生成代码模拟硬件地址翻译，TLB entry 中还包含 host 地址 `addend`、MMIO 标志等模拟器特有的信息。
