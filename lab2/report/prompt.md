# 实验 2 提示词记录

**实验者**：2313551-王海同、2413354-李文俊、2410248-戴康宁  
**日期**：2026-10-19  
**AI 编程工具**：Codex（OpenAI 桌面版）  
**说明**：以下记录按实验练习顺序整理，包含最终提示词、迭代过程中用于排查 bug 的提示词，以及对应的人工处理结果。

---

## 1. 环境与启动排查

### 1.1 QEMU/OpenSBI 启动问题

```text
我在 WSL2 的 Ubuntu 里用 qemu-system-riscv64 运行 ucore lab2。
执行 make qemu 后只能看到 OpenSBI 的输出，Domain0 Next Address 是 0x0，
内核没有输出 "(THU.CST) os is loading ..."。

请分析：
1. -bios default、-kernel、-device loader 三者的区别；
2. 为什么新版 OpenSBI 不会自动跳到 0x80200000；
3. 在保留 OpenSBI 的前提下，怎样让 QEMU 正确加载并跳转到内核；
4. 如果改用 -kernel，raw binary 的加载地址和链接地址有什么关系。
请给出可复现的 QEMU 命令和 Makefile 修改建议。
```

**结果**：确认新版 QEMU 自带的 OpenSBI 不会因为 `-device loader` 自动设置 `Next Address`；将 `qemu/debug/test` 目标改为 `-kernel bin/ucore.img` 后可以启动。

### 1.2 打开分页后取指异常

```text
QEMU 在 OpenSBI 之后进入内核，但内核没有任何输出。
用 -d int,guest_errors 看到：
cause=0xc, epc=0x80200038, tval=0x80200038, desc=exec_page_fault

我怀疑是 entry.S 里 csrw satp 打开 Sv39 后，PC 仍然是物理地址
0x80200038，而启动页表只映射了 0xffffffffc0000000 以上的高地址。
请解释这个现象，并给出两种修复方案：
（1）在启动页表中增加恒等映射；
（2）先建立低地址映射再跳转。
请说明哪种方案更适合本实验，以及页表项应该放在哪个 VPN2 索引。
```

**结果**：采用方案（1），在启动页表 VPN2=2 的位置增加 `0x80000000–0xbfffffff` 的 1 GiB 恒等映射，PTE 为 `(0x80000 << 10) | 0xcf`。这样 `csrw satp` 后物理 PC 仍能取指，随后 `jr` 跳到高地址虚拟入口。

---

## 2. 功能模块一：`page_init()`

### 2.1 实现提示词

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

### 2.2 编译错误修复提示词

```text
编译 pmm.c 时报错：
error: 'end' undeclared (first use in this function)

我知道 end 是链接脚本里的符号，但为什么不能在 C 文件顶层直接使用？
请说明链接符号和普通全局变量的区别，并给出在 page_init() 中正确声明它的方式。
```

**结果**：在 `page_init()` 内加 `extern char end[];`。

### 2.3 验证提示词

```text
请根据 tools/test_lab2.c 中的 test_page_init() 逐项检查我的 page_init()：
1. va_pa_offset 是否等于 PHYSICAL_MEMORY_OFFSET；
2. npage 是否等于 PPN(get_memory_base()+get_memory_size())；
3. pages 是否非空、页对齐、位于 ROUNDUP(end, PGSIZE) 之后；
4. PageReserved(pa2page(PADDR(end))) 是否为真；
5. PageReserved(pa2page(PADDR(pages))) 是否为真；
6. pa2page(page2pa(q)) == q 是否成立。
如果有不满足的项，请指出原因和修改方法。
```

---

## 3. First-Fit 理解（思考题）

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

**人工结论**：First-Fit 从链表头找第一个满足大小的块；释放时按地址插入并合并前后相邻块。改进方向包括 Best-Fit、按大小分桶、平衡树索引、Buddy System、Slab/SLUB、per-CPU 缓存等。

---

## 4. 功能模块二：Best-Fit

### 4.1 实现提示词

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

### 4.2 测试失败排查提示词

```text
best_fit_check() 在下面这段失败：
free_pages(p0 + 1, 2);
free_pages(p0 + 4, 1);
assert(alloc_pages(4) == NULL);
assert((p1 = alloc_pages(1)) != NULL);
assert(alloc_pages(2) != NULL);
assert(p0 + 4 == p1);   // 我的实现返回了 p0+1

请分析：
1. 为什么 Best-Fit 应该选择 p0+4；
2. 我的实现是否退化成了 First-Fit；
3. 如何保证大小相同时选择地址较低的块；
4. 拆分后剩余块应该怎样插入链表。
请给出修改前后的代码对比。
```

**结果**：修改为扫描整个链表，维护 `best`，只在 `page->property < best->property` 时更新；拆分出的剩余块用统一的按地址插入函数插入。

### 4.3 合并逻辑排查提示词

```text
我的 best_fit_free_pages() 在释放一个与前驱相邻的块后，后继合并没有生效。
请分析 list_prev/list_next 在按地址有序链表中的含义，并说明：
1. 为什么与前驱合并后必须更新 base；
2. 为什么合并前驱后还要重新取 list_next(&base->page_link)；
3. 如何避免重复 list_del 或访问已删除结点。
```

**结果**：合并前驱后执行 `base = p`，再检查新的 `base` 的后继，保证前后两个方向都能合并。

---

## 5. 功能模块二：Buddy System

### 5.1 实现提示词

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

### 5.2 测试提示词

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

### 5.3 Buddy 合并边界排查提示词

```text
我的 Buddy 实现在测试区域边界附近可能把相邻的 Best-Fit 空闲块误认为 buddy。
现象是 buddy_self_test() 之后 Best-Fit 的空闲链表被破坏。
请分析：
1. buddy_region_start/buddy_region_end 应该怎样设置；
2. 合并前需要检查哪些条件（区域范围、PageProperty、property 大小、块完整性）；
3. 为什么不能只检查 buddy 是否 PageProperty；
4. 如何保证测试结束后把描述符恢复成 Best-Fit 能接受的状态。
```

**结果**：增加区域边界检查；合并前同时检查 buddy 在管理范围内、buddy+size 不越界、`PageProperty` 为真且 `property == size`；测试结束后清除 flags/property/ref，再调用当前 Best-Fit 的 `free_pages()` 归还区域。

---

## 6. 扩展任务：SLUB

### 6.1 实现提示词

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

### 6.2 测试提示词

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

### 6.3 SLUB 页泄漏排查提示词

```text
slub_self_test() 释放所有对象后，nr_free_pages() 没有回到基线。
请分析：
1. 空 slab 什么时候应该归还给页分配器；
2. 从类链表摘除 slab 的顺序为什么重要；
3. 对象空闲链表指针存放在对象内部是否会与用户数据冲突；
4. 如何定位哪个大小类或哪个 slab 没有释放。
请给出排查步骤和修复代码。
```

**结果**：在 `inuse == 0` 时先从大小类链表摘下 slab，再保存 `page`、清空 slab 字段并调用 `free_pages(page, 1)`；测试最终确认页数恢复到基线。

---

## 7. Challenge：物理内存范围获取

```text
如果 OS 无法提前知道当前硬件的可用物理内存范围，请问有哪些办法让 OS 获取？
请从以下角度比较：
1. BIOS e820、UEFI GetMemoryMap、ACPI、RISC-V DTB/OpenSBI；
2. 内存控制器、SPD、PCI 配置空间；
3. 主动读写探测的风险和限制；
4. 如何区分可用 DRAM、MMIO 空洞、固件保留区、ACPI 保留区；
5. 内存热插拔和虚拟化环境下的获取方式。
请结合本实验的 DTB 流程说明。
```

---

## 8. 报告整理与自检

```text
请根据实验手册的要求检查报告是否覆盖：
1. 本章整体逻辑和实现顺序；
2. 每个功能的核心函数/模块理解；
3. 每个练习的 spec 和 prompt；
4. AI 生成代码过程中出现的 bug 及解决方法；
5. 本实验知识点与 OS 原理的对应、关系和差异；
6. OS 原理中重要但实验未覆盖的知识点；
7. First-Fit/Best-Fit 的改进空间；
8. Buddy/SLUB 的两份 prompt、测试覆盖和有效性；
9. Challenge 问题回答。
请列出缺失项，不要直接改写报告。
```

**自检结果**：以上各项均已在 `report.md` 中覆盖。
