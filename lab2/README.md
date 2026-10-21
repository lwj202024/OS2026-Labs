# Lab 2：物理内存和页表

本目录包含操作系统实验二的代码与实验报告。

## 目录结构

```text
lab2/
├── code/       # 完整 ucore lab2 代码
└── report/     # 实验报告与提示词记录
    ├── report.md
    └── prompt.md
```

## 实现内容

- `kern/mm/pmm.c`：`page_init()` 物理内存探测、`Page` 描述符数组和空闲区初始化。
- `kern/mm/best_fit_pmm.c`：Best-Fit 连续物理页分配器，并切换为默认 `pmm_manager`。
- `kern/mm/buddy_pmm.c`：Buddy System 页分配器（分裂/合并）及自测。
- `kern/mm/slub.c`：简化 SLUB/Slab 对象分配器（页级分配 + 对象缓存）及自测。
- `kern/init/entry.S`：启动页表增加临时恒等映射，适配新版 OpenSBI/QEMU。

## 编译运行

```bash
cd code
make
make qemu
```

评分测试：

```bash
make grade
```

本地验证结果：

```text
check page init: OK
check best fit:  OK
Total Score: 20/20
[Buddy] self-test PASS
[SLUB] self-test PASS
```

## 实验报告

- [实验报告](report/report.md)
- [提示词记录](report/prompt.md)
- [分支任务：QEMU 页表调试](report/debug.md)
- [双 GDB 调试脚本](debug/dual_debug.sh)
