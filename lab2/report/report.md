# 操作系统实验报告

## Lab 2：物理内存和页表

| 项目 | 内容 |
|------|------|
| **实验名称** | Lab 2：物理内存和页表 |
| **小组成员** | 2313551-王海同、2413354-李文俊、2410248-戴康宁 |
| **完成日期** | 2026-10-19 |
| **代码目录** | `lab2/code` |
| **报告目录** | `lab2/report` |

### 小组分工

| 成员 | 负责内容 |
|------|----------|
| 2313551-王海同 | 物理内存探测、`page_init()` 设计、实验报告整理 |
| 2413354-李文俊 | Best-Fit 分配器、Buddy System、SLUB、代码提交 |
| 2410248-戴康宁 | First-Fit 分析、测试用例、QEMU/GDB 验证 |

> 说明：本实验使用 AI 编程工具辅助完成代码生成、错误定位和报告润色。所有 AI 生成的代码都经过人工阅读、编译、运行和测试验证。

---

## 一、实验目的

1. 理解 RISC-V Sv39 分页机制，理解虚拟地址、物理地址、页号和页内偏移之间的关系。
2. 理解物理内存探测方法，能够从设备树（DTB）中获得可用物理内存的起始地址和大小。
3. 理解 `struct Page` 页描述符、空闲页链表和物理内存管理器 `pmm_manager` 的组织方式。
4. 理解并实现 First-Fit、Best-Fit 连续物理页分配算法。
5. 理解伙伴系统（Buddy System）的分配、分裂与合并过程。
6. 了解 SLUB/Slab 分配器的两层结构：页级分配 + 任意大小对象分配。
7. 学习使用 AI 辅助操作系统内核开发，并掌握“生成代码—编译—运行—定位—修正”的迭代方法。

---

## 二、实验环境与复现方法

### 2.1 实验环境

| 项目 | 版本/说明 |
|------|-----------|
| 宿主系统 | Windows 11 + WSL2 Ubuntu |
| 编译器 | `riscv64-unknown-elf-gcc 15.1.0` |
| 模拟器 | `qemu-system-riscv64 10.2.1` |
| 构建工具 | GNU Make 4.4.1 |
| 固件 | OpenSBI（QEMU `-bios default`） |
| 内核架构 | RISC-V 64 位，Sv39 三级页表 |

### 2.2 编译与运行

在 `lab2/code` 目录下执行：

```bash
make clean
make
make qemu
```

预期输出包含：

```text
(THU.CST) os is loading ...
Special kernel symbols:
  entry  0xffffffffc02000d6 (virtual)
  ...
Kernel executable memory footprint: 31KB
memory management: best_fit_pmm_manager
physical memory map:
  memory: 0x0000000008000000, [0x0000000080000000, 0x0000000087ffffff].
check_alloc_page() succeeded!
[Buddy] self-test PASS
[SLUB] self-test PASS
satp virtual address: 0xffffffffc0206000
satp physical address: 0x0000000080206000
```

评分脚本：

```bash
make grade
```

本地运行结果：

```text
  -check page init:                          OK
  -check best fit:                           OK
Total Score: 20/20
```

> **环境适配说明**：新版 QEMU 自带的 OpenSBI 对 `-device loader` 不会自动把 `Next Address` 设置为 `0x80200000`，因此本仓库的 `Makefile` 和 `tools/grade.sh` 使用 `-kernel bin/ucore.img` 启动内核；同时 `entry.S` 的启动页表增加了临时恒等映射，保证刚执行 `csrw satp` 后仍能按物理地址继续取指。这两处修改已在“Bug 与解决”一节说明。

---

## 三、本章节整体逻辑

本实验围绕“内核如何发现、描述、分配和回收物理内存”展开，整体调用链为：

```text
kern_init()
  -> dtb_init()                 // 从设备树取得物理内存 base/size
  -> pmm_init()
       -> init_pmm_manager()    // 选择并初始化物理内存管理器
       -> page_init()           // 建立 Page 描述符数组，划分保留区和空闲区
            -> init_memmap()    // 把空闲区交给具体分配器
       -> check_alloc_page()    // 运行分配/释放正确性测试
```

可以按以下顺序理解本章：

1. **发现内存**：OpenSBI 把设备树地址放在 `a1` 寄存器中，`entry.S` 保存到 `boot_dtb`；`dtb_init()` 解析 `memory` 节点的 `reg` 属性，得到物理内存起点和大小。
2. **描述内存**：`page_init()` 根据 `npage - nbase` 计算需要多少个 `struct Page`，把描述符数组放在内核镜像 `end` 之后，并先全部标记为保留。
3. **划分内存**：把 OpenSBI、内核镜像、页描述符数组所在页面排除，把描述符数组之后的完整页到物理内存末尾之间的区域交给分配器。
4. **管理内存**：`pmm_manager` 用函数指针统一 `init_memmap`、`alloc_pages`、`free_pages`、`nr_free_pages`、`check` 等接口；First-Fit 和 Best-Fit 只是同一接口的不同实现。
5. **验证内存**：`check_alloc_page()` 通过连续分配、拆分、释放、合并等场景验证分配器；本实验还增加了 Buddy 和 SLUB 的自测。

### 3.1 关键地址关系

| 名称 | 含义 |
|------|------|
| `KERNBASE = 0xFFFFFFFFC0200000` | 内核高地址虚拟基址 |
| `KERNEL_BEGIN_PADDR = 0x80200000` | 内核物理起始地址 |
| `PHYSICAL_MEMORY_OFFSET = 0xFFFFFFFF40000000` | 虚拟地址与物理地址的固定偏移 |
| `va_pa_offset` | 运行时软件保存的虚实偏移，`PADDR(kva) = kva - va_pa_offset` |
| `nbase = DRAM_BASE / PGSIZE` | DRAM 第一个物理页号（`0x80000`） |
| `npage = PPN(mem_end)` | 可管理物理页号上界（默认 `0x88000`） |
| `pages` | `struct Page` 描述符数组的虚拟地址 |
| `page2ppn(page) = page - pages + nbase` | 描述符下标到物理页号的转换 |
| `pa2page(pa) = &pages[PPN(pa) - nbase]` | 物理地址到描述符的转换 |

需要特别区分：

- **虚拟地址**：内核代码中使用的 `0xffffffffc0...` 地址；
- **物理地址**：DRAM 中真实的 `0x80200000`、`0x88000000` 等地址；
- **物理页号 PPN**：物理地址右移 12 位；
- **描述符地址**：`struct Page` 数组元素的地址，不是它所描述的物理页地址。

---

## 四、功能模块与核心函数

### 4.1 `page_init()`：物理内存探测与页描述符初始化

`page_init()` 位于 `kern/mm/pmm.c`，由 `pmm_init()` 调用。它不接收参数，也不返回结果，但需要正确设置 `va_pa_offset`、`npage`、`pages`，并完成空闲区注册。

#### 实现步骤

1. 调用 `get_memory_base()` 和 `get_memory_size()` 取得 DTB 中的内存信息。
2. 如果 `base == 0` 或 `size == 0`，说明 DTB 无效，调用 `panic()` 终止初始化。
3. 设置 `va_pa_offset = PHYSICAL_MEMORY_OFFSET`。启动页表把高地址 `0xffffffffc0000000` 起映射到物理 `0x80000000`，所以偏移是固定的。
4. 计算 `mem_end = mem_base + mem_size`，并令 `npage = PPN(mem_end)`。`npage` 是第一个超出可管理范围的物理页号。
5. 计算描述符数组的起点：
   ```c
   size_t nr_pages = npage - nbase;
   pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);
   uintptr_t pages_end = ROUNDUP((uintptr_t)pages +
                                  nr_pages * sizeof(struct Page), PGSIZE);
   ```
   其中 `end` 是链接脚本提供的内核镜像结束虚拟地址，必须先按页向上对齐。
6. 将 `[nbase, npage)` 对应的所有 `Page` 描述符先置为保留状态：
   ```c
   p->flags = 0;
   p->property = 0;
   set_page_ref(p, 0);
   SetPageReserved(p);
   ```
   这样在内核镜像、描述符数组等页面被排除之前，任何分配器都不会使用它们。
7. 计算真正可分配的空闲区：
   ```c
   uintptr_t free_start = PADDR(pages_end);
   uintptr_t free_end = ROUNDDOWN(mem_end, PGSIZE);
   if (free_start < free_end) {
       init_memmap(pa2page(free_start),
                   (free_end - free_start) / PGSIZE);
   }
   ```
   `free_start` 必须用 `PADDR` 转成物理地址，因为 `pages_end` 是虚拟地址；`free_end` 向下对齐，避免把不足一页的尾部交给分配器。若没有完整页，则不调用 `init_memmap()`。

#### 为什么描述符数组要放在 `end` 之后

`end` 是内核 `.bss` 段的结束地址。描述符数组不是内核镜像的一部分，不能被链接器分配；它在运行时紧跟在内核之后，因此从 `end` 向上按页对齐开始。数组本身占用 `nr_pages * sizeof(struct Page)` 字节，必须再次按页向上对齐，之后的页面才可以安全地加入空闲区。

#### 关键边界

- `npage <= nbase`：说明 DTB 报告的范围不包含 DRAM，必须 `panic`。
- `pages_end >= KERNTOP`：说明描述符数组超出了内核映射/可管理范围，必须 `panic`。
- 内核镜像尾页：`end` 所在的整页必须保持保留状态；测试会检查 `PageReserved(pa2page(PADDR(end)))`。
- 描述符数组首地址：测试会检查 `pages` 页对齐且位于 `ROUNDUP(end, PGSIZE)` 之后，并检查其所在页仍为保留状态。

#### 核心代码

```c
static void page_init(void) {
    extern char end[];
    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();

    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: invalid physical memory information from DTB");
    }

    va_pa_offset = PHYSICAL_MEMORY_OFFSET;
    uint64_t mem_end = mem_base + mem_size;
    npage = PPN(mem_end);

    size_t nr_pages = npage - nbase;
    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);
    uintptr_t pages_end = ROUNDUP((uintptr_t)pages +
                                  nr_pages * sizeof(struct Page), PGSIZE);

    for (size_t i = 0; i < nr_pages; i ++) {
        struct Page *p = &pages[i];
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
        SetPageReserved(p);
    }

    uintptr_t free_start = PADDR(pages_end);
    uintptr_t free_end = ROUNDDOWN(mem_end, PGSIZE);
    if (free_start < free_end) {
        init_memmap(pa2page(free_start),
                    (free_end - free_start) / PGSIZE);
    }
}
```

### 4.2 `pmm_manager`：物理内存管理器的统一接口

`struct pmm_manager` 定义在 `kern/mm/pmm.h`：

```c
struct pmm_manager {
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base, size_t n);
    struct Page *(*alloc_pages)(size_t n);
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);
    void (*check)(void);
};
```

`pmm.c` 中的 `init_memmap()`、`alloc_pages()`、`free_pages()`、`nr_free_pages()` 只是转发到当前 `pmm_manager` 的函数指针。这样 First-Fit、Best-Fit、Buddy 等算法可以共享同一套上层接口。本实验默认选择：

```c
pmm_manager = &best_fit_pmm_manager;
```

### 4.3 First-Fit 分配器：`default_pmm.c`

First-Fit 的核心思想是：维护一个按物理地址有序的空闲块链表，分配时从头扫描，找到第一个大小满足要求的空闲块。

#### `default_init()`

```c
list_init(&free_list);
nr_free = 0;
```

初始化空闲链表头，空闲页总数为 0。

#### `default_init_memmap(base, n)`

- 对 `[base, base+n)` 中每个页，断言其处于保留状态；
- 清除 `flags` 和 `property`，把引用计数置 0；
- 将首块 `base->property = n`，并置 `PG_property`，表示这是一个大小为 `n` 的空闲块的头页；
- `nr_free += n`；
- 按物理地址顺序把该块插入 `free_list`。

#### `default_alloc_pages(n)`

- 若 `n > nr_free`，直接返回 `NULL`；
- 从头遍历空闲链表，找到第一个 `property >= n` 的块；
- 从链表删除该块；
- 若块大于 `n`，把剩余部分 `page + n` 作为新空闲块，`property = old_property - n`，重新插入链表；
- `nr_free -= n`；
- 清除被分配块头页的 `PG_property`，返回 `page`。

#### `default_free_pages(base, n)`

- 对每个页断言 `!PageReserved && !PageProperty`，清除 `flags`，引用计数置 0；
- 设置 `base->property = n`，置 `PG_property`，`nr_free += n`；
- 按地址顺序插入空闲链表；
- 检查前一个空闲块：若 `prev + prev->property == base`，合并，删除 `base` 结点；
- 检查后一个空闲块：若 `base + base->property == next`，合并，删除 `next` 结点。

#### First-Fit 是否有改进空间

有。First-Fit 的优点是实现简单、分配开销较低；缺点是：

1. **外部碎片**：总是从低地址开始切分，容易把大块切碎，剩下很多小块。
2. **扫描是 O(n)**：空闲块很多时，每次分配都要从头扫描。
3. **没有大小索引**：无法快速找到“刚好够用”的块。
4. **合并策略简单**：只在释放时合并相邻块，无法主动整理碎片。

常见改进：

- 使用 **Best-Fit** 或 **Worst-Fit**，改变选择策略；
- 按块大小建立 **分离空闲链表（segregated free list）** 或 **位图/多级索引**，把扫描降到近似 O(1)；
- 使用 **Buddy System** 以 2 的幂为单位管理，便于快速分裂和合并；
- 使用 **Slab/SLUB** 管理固定大小的小对象，减少页内碎片；
- 增加 **per-CPU 缓存**、延迟合并、边界标记（boundary tag）等工程优化；
- 对多核环境增加锁或每 CPU 独立空闲链表。

### 4.4 Best-Fit 分配器：`best_fit_pmm.c`

Best-Fit 的核心思想是：扫描整个空闲链表，选择所有 `property >= n` 的块中 `property` 最小的那个；若大小相同，选择先遇到的（地址较低）块。

#### 数据结构

```c
static free_area_t best_fit_free_area;
#define free_list (best_fit_free_area.free_list)
#define nr_free   (best_fit_free_area.nr_free)
```

`free_area_t` 包含链表头和空闲页计数。块的头页用 `property` 记录块大小，并用 `PG_property` 标记。

#### 初始化

`best_fit_init()` 初始化链表并把 `nr_free` 置 0。`best_fit_init_memmap()` 与 First-Fit 类似：清除保留位、设置头页 `property = n`、插入按地址有序的链表。

#### 分配

```c
struct Page *best = NULL;
list_entry_t *le = &free_list;
while ((le = list_next(le)) != &free_list) {
    struct Page *page = le2page(le, page_link);
    if (page->property >= n &&
        (best == NULL || page->property < best->property)) {
        best = page;
    }
}
```

找到后：

- 从链表删除 `best`；
- 若 `best->property > n`，把剩余部分 `best + n` 作为新的空闲块插入；
- 清除 `best` 的 `PG_property`，`nr_free -= n`；
- 返回 `best`。

#### 释放与合并

`best_fit_free_pages()` 与 First-Fit 类似：清除页状态、设置头页 `property = n`、按地址插入链表，然后分别检查前驱和后继，若物理地址连续则合并。因为链表按地址有序，前驱就是物理上可能相邻的低地址块，后继就是高地址块。

#### 测试中的关键点

框架测试 `best_fit_check()` 会构造如下场景：

- 分配 5 页，然后释放 `p0+1` 的 2 页和 `p0+4` 的 1 页；
- 请求 4 页应失败（没有 4 页连续块）；
- 请求 1 页时必须返回 `p0+4`，而不是 `p0+1`，因为 1 页块比 2 页块更“贴合”；
- 随后请求 2 页返回 `p0+1`；
- 最后释放整个 5 页区域，再请求 5 页应成功。

这正是 Best-Fit 与 First-Fit 的关键区别：First-Fit 会返回 `p0+1`，而 Best-Fit 会选择大小为 1 的块。

#### Best-Fit 的改进空间

当前实现为了可读性使用单链表线性扫描，时间复杂度为 O(n)，插入也是 O(n)。可以进一步改进：

1. **按大小分桶**：维护多个按大小区间划分的空闲链表，先定位可能的桶，再在桶内找最小合适块。
2. **平衡树索引**：用红黑树/区间树按 `property` 维护空闲块，查找、插入、删除均为 O(log n)。
3. **位图/多级位图**：用位图记录各阶空闲块，快速定位。
4. **分割阈值**：如果剩余部分太小，考虑整块分配，避免产生无法使用的小碎片。
5. **延迟合并/批量合并**：减少频繁插入删除。
6. **per-CPU 空闲链表与锁**：适应多核。
7. **与 Slab 结合**：小对象不走页级 Best-Fit，减少内部碎片和扫描开销。

### 4.5 Buddy System 扩展

#### 设计

Buddy System 把物理内存划分为大小为 `2^order` 页的块。本实现支持 `order = 0..12`，即 1 页到 4096 页（16 MiB）。

核心数据结构：

```c
static free_area_t buddy_free_area[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free;
static struct Page *buddy_region_start;
static struct Page *buddy_region_end;
```

- 每个 order 一个空闲链表；
- 空闲块的头页 `property = 1 << order`；
- `buddy_region_start/end` 限定合并范围，防止 Buddy 误合并当前管理器范围之外的页；
- `buddy_nr_free` 记录空闲页总数。

#### 初始化：把任意区间拆成对齐的 2 的幂块

`buddy_init_memmap(base, n)` 不能假设 `base` 一定按最大 order 对齐。做法是从区间起点开始，每次找“当前地址对齐且不超过剩余长度”的最大 2 的幂块：

```c
size_t index = (base - pages) + offset;
int order = 0;
while (order < BUDDY_MAX_ORDER &&
       (1UL << (order + 1)) <= remain &&
       (index & ((1UL << (order + 1)) - 1)) == 0) {
    order ++;
}
```

这样任意 `[base, base+n)` 都能被完整分解为若干个自然对齐的 Buddy 块。

#### 分配

1. 计算满足 `2^order >= n` 的最小 order；
2. 从该 order 向上找第一个非空链表；
3. 取出块，不断二分：左半部分继续使用，右半部分插入低一阶的空闲链表；
4. 直到达到目标 order，清除头页的 `PG_property`，返回块首地址。

#### 释放与合并

释放时：

1. 根据 `base->property` 恢复块大小和 order；
2. 计算伙伴地址：`buddy_index = index ^ size`；
3. 若伙伴在管理范围内、是空闲块且大小相同，则从链表删除并合并；
4. order 加 1，继续向更高阶合并；
5. 最后把合并后的块插入对应 order 的空闲链表，更新 `buddy_nr_free`。

#### 自测

`buddy_self_test()` 从当前 Best-Fit 分配器申请 64 页作为测试区域，然后：

- 依次分配 1、2、4、8、16、32 页；
- 检查每个返回块按自身大小对齐；
- 用 `used[64]` 检查所有分配块互不重叠；
- 逆序释放，覆盖左右伙伴合并；
- 检查 `buddy_nr_free_pages() == 64`；
- 遍历所有 order 的空闲链表，检查 `property`、链表计数和总页数一致；
- 最后把测试区域还给 Best-Fit，确保不影响后续测试。

测试覆盖了：对齐、分裂、合并、不同 order、边界、计数一致性、区域外误合并等核心行为。

### 4.6 SLUB 扩展

#### 设计

SLUB 采用两层结构：

- **第一层**：从 `pmm_manager` 申请整页；
- **第二层**：把一页切成固定大小对象，用对象内部的前 8 字节保存空闲链表指针。

本实现支持 8 个大小类：

```c
16, 32, 64, 128, 256, 512, 1024, 2048
```

核心结构：

```c
struct slub_slab {
    struct Page *page;
    size_t objsize;
    void *free_list;
    unsigned int inuse;
    unsigned int total;
    struct slub_slab *next;
};
```

每个大小类维护一个 slab 链表。分配时先找有空闲对象的 slab；没有就调用 `alloc_pages(1)` 新建一个 slab，并把页内对象串成空闲链表。释放时把对象压回 slab 的空闲链表；当 `inuse == 0` 时，把整个 slab 从类链表摘下并调用 `free_pages(page, 1)` 归还页。

#### 自测

`slub_self_test()`：

- 对每个大小类分配 8 个对象；
- 检查对象地址按大小类对齐；
- 检查同一类中的对象互不相同；
- 向对象写入不同模式并逐字节校验，验证对象之间没有重叠；
- 释放所有对象，检查 `nr_free_pages()` 回到测试前的基线；
- 检查超过最大对象大小的请求返回 `NULL`；
- 遍历 slab 链表检查 `inuse <= total` 等不变量。

测试覆盖了：多大小类、对齐、对象独立性、内容完整性、空 slab 回收、页数泄漏、超大请求等边界情况。

---

## 五、每个练习的 Spec 与 Prompt

> 本节记录实验手册要求（Spec）以及实际使用的提示词。完整的提示词汇总也保存在 `lab2/report/prompt.md`。

### 5.1 功能模块一：`page_init()` 物理内存探测与页描述符初始化

#### Spec 提炼

- 实现 `static void page_init(void)`，位于 `kern/mm/pmm.c`；
- 从 `get_memory_base()`、`get_memory_size()` 取得 DTB 内存信息；
- 正确设置 `va_pa_offset`、`npage`、`pages`；
- 区分虚拟地址、物理地址、物理页号和 `Page` 描述符地址；
- 描述符数组放在 `end` 之后并按页对齐；
- 先保留所有描述符对应页面，排除 OpenSBI、内核镜像、描述符数组；
- 空闲区上界按页向下对齐，只把完整页交给 `init_memmap()`；
- DTB 无效时 `panic()`；没有完整页时不得调用 `init_memmap()`；
- 输出探测到的物理内存范围；
- 保留 `//lab2` 注释，把 `YOUR CODE` 替换为学号。

#### 使用的 Prompt（最终版）

```text
你是 ucore/RISC-V lab2 的助教。请先阅读 kern/mm/pmm.c、kern/mm/pmm.h、
kern/mm/memlayout.h、kern/mm/mmu.h、kern/driver/dtb.c 和 tools/kernel.ld，
理解 KERNBASE、PHYSICAL_MEMORY_OFFSET、PADDR、page2pa/pa2page、nbase、npage
以及 struct Page 的含义。

任务：实现 kern/mm/pmm.c 中的 static void page_init(void)。

要求：
1. 从 get_memory_base()/get_memory_size() 读取 DTB 报告的内存起点和大小；
   如果 base 或 size 为 0，调用 panic()。
2. 设置 va_pa_offset = PHYSICAL_MEMORY_OFFSET。
3. 计算 mem_end = base + size，npage = PPN(mem_end)，并检查 npage > nbase。
4. pages 从链接符号 end 之后按 PGSIZE 向上对齐开始；
   描述符数量为 npage - nbase，数组末尾也要按 PGSIZE 向上对齐。
5. 先把 [nbase, npage) 对应的所有 Page 描述符设置为 reserved，
   清除 flags/property，引用计数置 0。
6. 用 PADDR(pages_end) 得到空闲区物理起点，用 ROUNDDOWN(mem_end, PGSIZE)
   得到空闲区物理终点；只有 free_start < free_end 时才调用
   init_memmap(pa2page(free_start), (free_end - free_start) / PGSIZE)。
7. 输出 physical memory map，格式与实验手册一致。
8. 不要修改函数接口和其他函数；保留 //lab2 注释，把 YOUR CODE 替换为 2413354。
9. 请先解释虚拟地址/物理地址/页号/描述符地址的关系，再给出完整代码，
   最后给出编译和 QEMU 验证步骤。
```

#### AI 生成后出现的问题

- **编译错误：`end` undeclared**。原因：`end` 是链接脚本定义的符号，不是普通全局变量，需要在函数内用 `extern char end[];` 声明。修正后编译通过。
- **QEMU 在 `csrw satp` 之后取指异常**。最初 `entry.S` 只建立了高地址 1 GiB 映射，打开分页后 PC 仍停在物理地址 `0x80200038`，Sv39 把它当作低地址虚拟地址，页表项为 0，触发 `exec_page_fault`。修正：在启动页表中增加 `0x80000000–0xbfffffff` 的临时恒等映射（VPN2 = 2），让刚打开分页时的物理 PC 能继续取指，再通过 `jr` 跳到高地址虚拟入口。

### 5.2 功能模块二：First-Fit 理解（思考题）

#### Spec 提炼

- 阅读 `kern/mm/default_pmm.c` 中的 `default_init`、`default_init_memmap`、`default_alloc_pages`、`default_free_pages`；
- 用自己的话说明 First-Fit 如何初始化空闲页、分配连续页、回收页面并合并相邻空闲块；
- 回答 First-Fit 是否有改进空间。

#### 使用的 Prompt

```text
请逐行解释 ucore lab2 的 kern/mm/default_pmm.c 中 default_init、
default_init_memmap、default_alloc_pages、default_free_pages 四个函数。
重点说明：
1. free_area_t、free_list、nr_free 的作用；
2. Page.property 和 PG_property 如何表示空闲块；
3. First-Fit 分配时如何找到第一个合适的块、如何拆分；
4. 释放时如何按地址插入链表、如何与前后相邻块合并；
5. 这个实现的时间复杂度、外部碎片问题，以及可以怎样改进。
请结合一个具体的分配/释放例子说明。
```

#### 理解要点

First-Fit 的初始化把整段连续空闲内存作为一个块插入链表；分配时从链表头开始找第一个 `property >= n` 的块，必要时切分；释放时把页重新插入按地址排序的链表，并检查前后物理相邻块进行合并。它的优点是简单、分配快；缺点是大块容易被切碎、扫描是线性的、可能产生较多外部碎片。改进方向包括 Best-Fit、按大小分桶、平衡树索引、Buddy System、Slab/SLUB、per-CPU 缓存等。

### 5.3 功能模块二：Best-Fit 分配器

#### Spec 提炼

- 在 `kern/mm/best_fit_pmm.c` 中实现完整的 Best-Fit 模块；
- 只提供 `basic_check()` 和 `best_fit_check()`，不得修改这两个测试函数；
- 自行设计内部状态、管理函数和 `best_fit_pmm_manager`；
- 在 `pmm.c` 的 `init_pmm_manager()` 中切换为 `best_fit_pmm_manager`；
- 算法时空复杂度不作要求，能通过测试即可；
- 报告需提交 prompt，并回答 Best-Fit 的改进空间。

#### 使用的 Prompt（最终版）

```text
请阅读 ucore lab2 的 kern/mm/default_pmm.c 和 kern/mm/pmm.h，
理解 struct pmm_manager、free_area_t、Page.property、PG_property 和
list.h 的链表操作。

任务：在 kern/mm/best_fit_pmm.c 中实现完整的 Best-Fit 物理内存管理器。
文件目前只有 basic_check() 和 best_fit_check() 两个测试函数，不能修改它们。
请自行补充：
1. static free_area_t，以及 free_list/nr_free 宏；
2. best_fit_init()：初始化空闲链表和 nr_free；
3. best_fit_init_memmap(base, n)：清除 reserved，设置头页 property=n，
   按物理地址有序插入空闲链表，nr_free += n；
4. best_fit_alloc_pages(n)：遍历整个空闲链表，选择 property >= n 且
   property 最小的块；大小相同时选择先遇到的（地址较低的）块；
   若块大于 n，把剩余部分作为新空闲块插入；清除头页 PG_property；
5. best_fit_free_pages(base, n)：清除页状态，设置头页 property=n，
   按地址插入链表，并与前驱/后继物理相邻空闲块合并；
6. best_fit_nr_free_pages()；
7. const struct pmm_manager best_fit_pmm_manager，正确绑定所有函数指针；
8. 在 kern/mm/pmm.c 的 init_pmm_manager() 中改为
   pmm_manager = &best_fit_pmm_manager。

约束：
- 保留 //lab2 注释，把 YOUR CODE 替换为 2413354；
- 不修改 basic_check/best_fit_check；
- 代码要能通过 make 和 make grade。
请给出完整实现，并解释为什么测试中 alloc_pages(1) 应返回 p0+4 而不是 p0+1。
```

#### AI 生成后出现的问题

- 最初版本使用“第一个满足 `property >= n` 的块”，实际退化成 First-Fit，测试中 `alloc_pages(1)` 返回了 `p0+1` 而不是 `p0+4`。修正：扫描整个链表，显式维护 `best`，只有 `page->property < best->property` 时才更新；大小相同时不更新，从而保留地址较低的块。
- 拆分后剩余块没有正确设置 `PG_property` 或没有按地址插入，导致后续合并找不到块。修正：拆分时设置 `remainder->property = old_property - n`、`SetPageProperty(remainder)`，并用统一的 `best_fit_insert_free_block()` 按地址插入。
- 释放时先插入再合并，若前驱合并后 `base` 改变，必须用新的 `base` 继续检查后继。修正：合并前驱后更新 `base = p`，再检查 `list_next(&base->page_link)`。

### 5.4 功能模块二：Buddy System

#### Spec 提炼

- 参考 ucore 物理内存管理框架，从零生成 Buddy System 分配模块并接入内核；
- 块大小为 2 的幂；
- 自行设计测试函数；
- 报告提交两份 prompt（实现 + 测试），说明测试覆盖的核心行为和边界情况。

#### Prompt 1：实现 Buddy System

```text
请参考 ucore lab2 的 pmm_manager 框架，从零实现一个 Buddy System 物理页分配器，
放在 kern/mm/buddy_pmm.c/.h 中。

要求：
1. 用 free_area_t 数组维护 0..BUDDY_MAX_ORDER 阶的空闲块，每个块大小为
   2^order 页；只在块头页保存 property=2^order 并设置 PG_property。
2. buddy_init()：初始化所有阶的空闲链表和空闲页计数。
3. buddy_init_memmap(base, n)：不能假设 base 按最大阶对齐；请把任意
   [base, base+n) 分解为自然对齐的 2 的幂块，逐个插入对应阶链表；
   初始化时断言输入页处于 reserved 状态，并清除 flags/property/ref。
4. buddy_alloc_pages(n)：找到满足 2^order >= n 的最小阶，向更高阶找空闲块；
   取出后不断二分，右半块插入低一阶链表，直到目标阶；返回块首地址。
5. buddy_free_pages(base, n)：根据块头 property 恢复阶，计算 buddy 地址
   （index ^ size），若 buddy 是空闲且同阶则合并，继续向高阶合并；
   必须限定在管理区域内，不能误合并区域外的页。
6. buddy_nr_free_pages() 和 buddy_check()：检查链表计数、property 和总页数。
7. 定义 const struct pmm_manager buddy_pmm_manager，但默认仍使用 Best-Fit；
   提供一个 buddy_self_test() 供内核启动时验证。
8. 保留 //lab2 注释，把 YOUR CODE 替换为 2413354。

请说明分裂/合并的边界条件，并给出完整代码。
```

#### Prompt 2：设计 Buddy 测试

```text
请为上面的 Buddy System 实现设计一组内核自测函数 buddy_self_test()，
要求不修改 Best-Fit 和其他模块，测试结束后把临时区域完整归还。

请覆盖：
1. 从当前 pmm 申请 64 页作为测试区域；
2. 依次分配 1、2、4、8、16、32 页，检查非空、按自身大小对齐；
3. 用位图/数组检查所有分配块互不重叠，且都在测试区域内；
4. 逆序释放，覆盖左伙伴和右伙伴两种合并方向；
5. 释放后检查 nr_free_pages() 恢复为 64；
6. 遍历每个 order 的空闲链表，检查 property == 2^order、链表计数正确、
   所有空闲页总数等于 64；
7. 检查不存在相邻的同阶空闲块（否则说明合并不完整）；
8. 测试区域边界：确保不会把区域外的页当作 buddy 合并；
9. 失败时输出明确信息，成功时输出 "[Buddy] self-test PASS"。
请给出测试代码和测试设计说明。
```

#### 测试覆盖与有效性

| 测试点 | 覆盖的行为 |
|--------|-----------|
| 1/2/4/8/16/32 页分配 | 不同 order、向上取整、分裂 |
| 返回地址对齐检查 | Buddy 块必须按自身大小对齐 |
| `used[64]` 位图 | 分配块不重叠、不越界 |
| 逆序释放 | 左伙伴/右伙伴合并 |
| `nr_free == 64` | 分配/释放计数平衡 |
| 每个 order 的链表检查 | `property`、链表计数、总页数一致 |
| 相邻同阶块检查 | 合并完整，没有可合并却未合并的块 |
| 区域边界检查 | 不会把测试区域外的页误当作 buddy |

这些测试能有效检验 Buddy 实现，因为 Buddy 最容易出错的地方正是“对齐、分裂、伙伴计算、跨阶合并、计数”；测试同时覆盖正常路径和边界路径。

### 5.5 扩展任务：SLUB

#### Spec 提炼

- 实现两层架构：页级分配 + 任意大小对象分配；
- 可简化，但要体现 SLUB 主体思想；
- 自行设计实现和测试；
- 报告提交两份 prompt，说明测试覆盖的核心行为和边界情况。

#### Prompt 1：实现 SLUB

```text
请在没有现成 SLUB 代码框架的情况下，为 ucore lab2 从零实现一个简化的
SLUB 对象分配器，放在 kern/mm/slub.c/.h 中。

要求：
1. 第一层使用现有的 alloc_pages()/free_pages() 从 pmm_manager 申请/归还页；
2. 第二层把一页切成固定大小对象，支持 16、32、64、128、256、512、1024、
   2048 字节等大小类；
3. 每个大小类维护 slab 链表；每个 slab 记录 page、objsize、free_list、
   inuse、total、next；
4. 新建 slab 时把页内对象串成空闲链表，空闲链表指针存放在对象内部前 8 字节；
5. slub_alloc(size)：向上取整到大小类，找到有空闲对象的 slab；没有则新建；
6. slub_free(ptr)：根据地址找到所属 slab，把对象压回空闲链表；
   当 slab 的 inuse 变为 0 时，从类链表摘下并 free_pages(page, 1)；
7. 使用静态 slab 描述符数组，不依赖 malloc；
8. 提供 slub_check() 检查 inuse <= total 等不变量；
9. 保留 //lab2 注释，把 YOUR CODE 替换为 2413354。
请说明两层结构、对象对齐和空 slab 回收策略。
```

#### Prompt 2：设计 SLUB 测试

```text
请为上面的 SLUB 实现设计 slub_self_test()，测试结束后所有页必须归还，
并且不破坏 Best-Fit 的空闲链表。

请覆盖：
1. 对每个大小类分配 8 个对象；
2. 检查返回地址按大小类对齐，且同一类对象互不相同；
3. 向每个对象写入不同模式（如 0x5a+c），逐字节校验内容；
4. 释放所有对象后，检查 nr_free_pages() 回到测试前基线；
5. 检查超过最大对象大小（如 4096）的请求返回 NULL；
6. 检查空 slab 会被归还给页分配器；
7. 遍历 slab 链表检查 inuse <= total、objsize 与大小类一致；
8. 失败时输出明确信息，成功时输出 "[SLUB] self-test PASS"。
请给出测试代码和设计说明。
```

#### 测试覆盖与有效性

| 测试点 | 覆盖的行为 |
|--------|-----------|
| 8 个大小类各 8 个对象 | 多类缓存、slab 新建、对象切分 |
| 地址对齐 | 对象按大小类对齐 |
| 对象互不相同 | 空闲链表没有重复弹出同一对象 |
| 写入/逐字节校验 | 对象不重叠、内存可写、内容完整 |
| 释放后 `nr_free_pages()` 回到基线 | 空 slab 回收、无页泄漏 |
| 4096 字节请求返回 NULL | 超大类边界 |
| `inuse <= total` 等不变量 | slab 内部计数正确 |

这些测试能有效检验 SLUB 实现，因为 SLUB 的核心风险是“对象重叠、空闲链表断裂、空 slab 不回收、页泄漏、对齐错误”；测试同时覆盖分配、释放、复用和资源归还。

### 5.6 Challenge：如果 OS 无法提前知道可用物理内存范围，如何获取？

#### 问题

如果 OS 无法提前知道当前硬件的可用物理内存范围，有什么办法让 OS 获取可用物理内存范围？

#### 回答

可以从以下几个层次获取：

1. **固件/引导程序提供内存映射**
   - x86：BIOS `e820`、UEFI `GetMemoryMap()`、ACPI 表；
   - RISC-V：OpenSBI 通过 `a1` 传递 DTB，解析 `/memory` 节点的 `reg`；
   - ARM：UEFI/DTB；
   - 虚拟机：hypervisor 提供的设备树或 paravirt 接口。
2. **读取内存控制器/SPD**
   - 通过内存控制器、SPD EEPROM、PCI 配置空间等获取 DRAM 容量和地址范围；
   - 需要区分 DRAM 和 MMIO 空洞。
3. **主动探测**
   - 在保证安全的前提下，对候选地址进行读写测试；
   - 必须避开 MMIO、固件、内核和已保留区域，不能随意写未知地址。
4. **ACPI/EFI 运行时服务**
   - 通过 ACPI `SRAT`、`E820`、UEFI memory map 获取可用、保留、ACPI、MMIO 等区域。
5. **内存热插拔/热更新**
   - 运行时通过 ACPI hotplug、设备树 overlay 等机制获知新内存。
6. **多源校验**
   - 固件报告的范围可能包含空洞或保留区，OS 需要建立自己的物理页描述符，只把“可用 RAM”加入分配器。

本实验采用第 1 种方式：OpenSBI 把 DTB 地址放在 `a1`，`entry.S` 保存到 `boot_dtb`，`dtb_init()` 解析 `memory` 节点的 `reg`，`page_init()` 再根据 `base/size` 建立页描述符和空闲区。这体现了“固件发现硬件、内核管理内存”的分工。

---

## 六、AI 生成代码中的 Bug 与解决

| 编号 | 现象 | 原因 | 解决方法 |
|------|------|------|----------|
| 1 | 编译报错 `'end' undeclared` | `end` 是链接脚本符号，不是普通变量 | 在 `page_init()` 内加 `extern char end[];` |
| 2 | 打开分页后 QEMU 在 `0x80200038` 触发 `exec_page_fault` | `csrw satp` 后 PC 仍是物理地址，Sv39 把它当低地址虚拟地址，启动页表未映射 | 启动页表增加 `0x80000000–0xbfffffff` 恒等映射，再跳到高地址虚拟入口 |
| 3 | 新版 QEMU `-device loader` 启动后 `Next Address = 0x0` | 新版 OpenSBI 不自动把 `Next Address` 设为 `0x80200000` | `Makefile`/`grade.sh` 改用 `-kernel bin/ucore.img` |
| 4 | Best-Fit 测试中 `alloc_pages(1)` 返回 `p0+1` | 实现退化成 First-Fit，只取第一个满足条件的块 | 扫描整个链表，选择 `property` 最小的块；同大小保留先遇到的块 |
| 5 | Best-Fit 拆分后找不到剩余块 | 剩余块没有设置 `PG_property`，或没有按地址插入 | 设置 `remainder->property` 并 `SetPageProperty`，统一用有序插入函数 |
| 6 | Best-Fit 释放后合并不完整 | 与前驱合并后 `base` 改变，仍用旧 `base` 检查后继 | 合并前驱后更新 `base = p`，再检查后继 |
| 7 | Buddy 可能误合并测试区域外的页 | 只检查 `PageProperty`，没有限制管理范围 | 增加 `buddy_region_start/end`，合并前检查 buddy 范围 |
| 8 | Buddy 初始化无法处理任意对齐的 `base` | 假设整个区间是一个 2 的幂块 | 用“最大对齐块”分解算法把区间拆成多个自然对齐块 |
| 9 | SLUB 释放空 slab 后页数不恢复 | 只把对象放回链表，没有把空 slab 还给页分配器 | `inuse == 0` 时从类链表摘下，调用 `free_pages(page, 1)` |
| 10 | SLUB 对象可能重叠 | 对象切分步长错误或空闲链表指针覆盖对象数据 | 每页按 `objsize` 切分，空闲链表指针只放在未分配对象的起始 8 字节 |

### 6.1 调试方法

- 用 `make` 先解决编译错误；
- 用 `make qemu` 观察内核输出和 panic 信息；
- 用 QEMU `-d int,guest_errors -D qemu.log` 查看异常原因（本次定位 `exec_page_fault` 时使用）；
- 用 GDB 在 `0x80200000` 设置断点，单步观察 `pc/satp/sp`；
- 用 `make grade` 运行注入的 `test_page_init()` 和 `test_best_fit()`；
- 对 Buddy/SLUB 使用自测中的 `assert` 和计数检查，逐步缩小问题范围。

---

## 七、测试与验证

### 7.1 编译与启动

`make` 成功编译所有源文件，`make qemu` 输出正常。关键输出：

```text
DTB Init
HartID: 0
DTB Address: 0x87e00000
Physical Memory from DTB:
  Base: 0x0000000080000000
  Size: 0x0000000008000000 (128 MB)
  End:  0x0000000087ffffff
DTB init completed
(THU.CST) os is loading ...
Special kernel symbols:
  entry  0xffffffffc02000d6 (virtual)
  etext  0xffffffffc02024a0 (virtual)
  edata  0xffffffffc0207018 (virtual)
  end    0xffffffffc0207c08 (virtual)
Kernel executable memory footprint: 31KB
memory management: best_fit_pmm_manager
physical memory map:
  memory: 0x0000000008000000, [0x0000000080000000, 0x0000000087ffffff].
check_alloc_page() succeeded!
[Buddy] self-test PASS
[SLUB] self-test PASS
satp virtual address: 0xffffffffc0206000
satp physical address: 0x0000000080206000
```

### 7.2 评分脚本

```text
lab2 injection complete
  -check page init:                          OK
  -check best fit:                           OK
Total Score: 20/20
```

### 7.3 测试覆盖总结

| 模块 | 测试内容 | 结果 |
|------|----------|------|
| `page_init()` | `va_pa_offset`、DTB 范围、`npage`、`pages` 对齐、内核尾页保留、描述符数组保留、`page2pa/pa2page` 往返 | PASS |
| Best-Fit | 基本分配/释放、5 页块拆分、1 页 Best-Fit 选择、合并、`nr_free` | PASS |
| Buddy | 1–32 页分配、对齐、不重叠、逆序释放、合并、计数、边界 | PASS |
| SLUB | 8 个大小类、对齐、对象独立性、内容、空 slab 回收、页数恢复 | PASS |

---

## 八、重要知识点与 OS 原理对应

| 本实验知识点 | OS 原理知识点 | 含义、关系与差异 |
|--------------|---------------|------------------|
| DTB 物理内存探测 | 硬件发现/固件接口 | 本实验通过 OpenSBI 传递 DTB；OS 原理中固件（BIOS/UEFI/ACPI）向 OS 报告内存映射。DTB 是 RISC-V 的具体实现形式。 |
| `struct Page` 描述符 | 物理页框管理 | 每个物理页一个描述符，记录引用计数、状态、空闲块大小；OS 原理中通常用 `page`/`page frame` 结构管理物理页。 |
| `free_area_t` 空闲链表 | 连续内存分配 | 本实验用双向链表管理空闲块；OS 原理中还有位图、分离链表、伙伴系统等。 |
| First-Fit | 首次适配算法 | 从低地址开始找第一个足够大的块；实现简单，但容易产生外部碎片。 |
| Best-Fit | 最佳适配算法 | 选择最小可用块；减少大块被切碎的概率，但扫描成本高、可能产生小碎片。 |
| 相邻块合并 | 碎片整理/合并 | 释放时合并物理相邻空闲块，降低外部碎片；OS 原理中还需要紧凑、页面回收等机制。 |
| `PADDR/KADDR`、`va_pa_offset` | 虚实地址转换 | 本实验内核使用固定偏移的高位直接映射；OS 原理中通过页表进行任意虚实映射。 |
| Sv39 三级页表、`satp` | 分页/MMU | 本实验启动时建立 1 GiB 大页映射；OS 原理中还会动态建立多级页表、支持 4 KiB/2 MiB/1 GiB 页。 |
| `sfence.vma` | TLB 刷新 | 修改 `satp` 或页表后需要刷新 TLB；OS 原理中上下文切换、页表修改都涉及 TLB 管理。 |
| Buddy System | 伙伴系统 | 按 2 的幂管理块，分裂/合并快；OS 原理中常用于物理页分配。 |
| SLUB | Slab 分配器 | 页级分配之上的对象缓存；OS 原理中用于内核小对象，减少内部碎片和分配开销。 |
| `PageReserved` | 保留区管理 | 内核、固件、MMIO 等页面不能被分配；OS 原理中需要维护保留内存和可用内存的边界。 |
| 引用计数 `ref` | 资源生命周期 | 本实验分配页时引用计数为 0，主要用于描述符状态；OS 原理中引用计数用于共享页、COW 等。 |

### 8.1 OS 原理中重要但本实验未覆盖的知识点

1. **进程与线程管理**：PCB/TCB、进程创建、调度算法、上下文切换。
2. **完整的用户态/内核态切换**：系统调用、异常/中断入口、陷阱帧、特权级检查。
3. **按需分页与页面置换**：缺页异常、页面回收、LRU/Clock、swap。
4. **用户地址空间**：每个进程独立页表、`fork`、`exec`、写时复制（COW）。
5. **多级页表的动态建立**：本实验只建立了启动大页映射，没有实现按需分配页表页。
6. **文件系统**：VFS、inode、目录、块设备缓存。
7. **同步与并发**：自旋锁、信号量、多核 per-CPU 分配器。
8. **设备驱动与 I/O**：中断控制器、DMA、块设备、字符设备。
9. **内存保护与安全**：页表权限位、用户/内核隔离、ASLR、栈保护。
10. **完整 SLUB**：per-CPU cache、NUMA、slab 回收、对象构造/析构、调试红区等。

---

## 九、总结与收获

本实验从“内核如何知道有多少内存”开始，逐步建立了物理内存管理的完整链路：

```text
DTB 探测 -> Page 描述符数组 -> 保留区划分 -> 空闲链表
         -> First-Fit/Best-Fit -> 分配/释放/合并
         -> Buddy System -> SLUB 对象分配
```

通过本实验，我们加深了对以下内容的理解：

1. **地址空间不是“一个地址”**：虚拟地址、物理地址、页号、页内偏移、描述符地址必须严格区分；`PADDR`、`page2pa`、`pa2page` 是连接它们的桥梁。
2. **内存管理是分层的**：页级分配器负责连续页，SLUB 负责小对象；不同层次解决不同粒度的碎片问题。
3. **测试是内核开发的一部分**：仅“能编译”不代表正确，必须通过分配/释放/合并/边界/计数等测试。
4. **AI 适合辅助，但不能替代验证**：AI 可以快速生成代码和解释，但地址转换、页表标志、对齐、边界条件等仍需要人工分析和 QEMU/GDB 验证。
5. **环境差异会影响启动流程**：新版 OpenSBI/QEMU 的 `-device loader` 行为与旧版不同，需要根据实际环境调整启动方式和启动页表。

---

## 附录 A：关键代码位置

| 文件 | 关键内容 |
|------|----------|
| `kern/mm/pmm.c` | `page_init()`、`init_pmm_manager()`、`pmm_init()` |
| `kern/mm/best_fit_pmm.c` | Best-Fit 初始化、分配、释放、合并、管理结构 |
| `kern/mm/buddy_pmm.c` | Buddy 初始化、分裂、合并、自测 |
| `kern/mm/slub.c` | SLUB 大小类、slab 管理、对象分配/释放、自测 |
| `kern/init/entry.S` | 启动页表、恒等映射、进入高地址虚拟入口 |
| `kern/driver/dtb.c` | DTB 解析、`get_memory_base()`、`get_memory_size()` |
| `kern/mm/default_pmm.c` | First-Fit 参考实现 |
| `tools/test_lab2.c` | 评分注入的 `test_page_init()`、`test_best_fit()` |

## 附录 B：完整提示词

完整提示词记录见同目录下的 [`prompt.md`](prompt.md)。其中按练习顺序整理了：

1. `page_init()` 实现与 bug 修复提示词；
2. First-Fit 理解提示词；
3. Best-Fit 实现与测试排查提示词；
4. Buddy System 实现与测试提示词；
5. SLUB 实现与测试提示词；
6. 物理内存范围获取（Challenge）提示词；
7. QEMU/GDB 环境排查提示词。

## 附录 C：分支任务调试报告

QEMU 页表查询与双 GDB 调试的完整记录见 [`debug.md`](debug.md)。
