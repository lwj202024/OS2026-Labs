# Lab 3 实际调试记录

本文件记录 2026-11-05 于 WSL2 / QEMU 中取得的实际测试结果。三项 `make grade` 均在正式项目中通过，总分 100/100；QEMU 启动方式为本机兼容性调整后的 `-kernel bin/ucore.img`。

| 阶段 | 实际问题 | 修改或验证 | 实际结果 |
|---|---|---|---|
| 汇编编译 | `unknown CSR 'sbadaddr'` | 将 CSR 名称改为 `stval` | 进入后续 C 编译 |
| Lab 2 继承 | `best_fit_pmm.c` 缺少 `free_list`、`nr_free` | 迁移 Lab 2 Best-Fit；补齐 `page_init()` | 测试全部可编译和链接 |
| 固件启动 | OpenSBI `Next Address = 0` | 本机 `grade.sh` 将 `-device loader` 改为 `-kernel bin/ucore.img` | `Next Address = 0x80200000`，但内核仍未输出 |
| 虚拟内存启用 | 页表只含高地址映射，启用 Sv39 后无法顺利继续取指 | 启动页表根索引 2 新增 1 GiB 恒等映射 | 三项测试首次全部 PASS |
| 正式验收 | 测试副本得到 100/100 | 将通过的源代码同步至正式目录再次 `make grade` | 再次取得 **100/100** |

实际截图参见 `report.md` 附录 A；没有实际截图的 GDB 单步或独立压缩指令专项测试不计入已完成验证。
