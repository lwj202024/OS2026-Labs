# 操作系统 Lab 3：RISC-V Trap 与中断异常处理

**小组成员：** 王海同（2313551）、李文俊（2413354）、戴康宁（2410248）

## 项目结构

- `kern/`：内核入口、物理内存初始化、Trap 和时钟驱动
- `libs/`：RISC-V CSR、SBI 及基础库
- `tools/`：构建、评分测试注入与 QEMU 启动脚本
- [**点击阅读完整图文实验报告**](report/report.md)；`report/images/`：8 张真实终端截图
- `report/debug.md`、`report/prompts.md`：调试记录与任务提示整理

## 本机验证

需要 `riscv64-unknown-elf-gcc`、GNU binutils、`qemu-system-riscv64`、`make`、`python3` 和 `timeout`。

```bash
make clean
make
make grade
```

在 WSL2 / GCC 14.2 / QEMU 10.2.1 / OpenSBI 1.8 环境下，正式目录实测：

```text
trap entry, register restore, and breakpoint EPC: PASS (35/35)
illegal instruction EPC advance and return: PASS (25/25)
re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 100/100
```

测试过程及截图见[完整实验报告](report/report.md)。

![Lab 3 正式评分截图](report/images/02_grade_100.png)

**本地无法显示图片时：** 请打开 `report/实验报告_离线完整版.html`，该文件包含全部图片，不依赖图片路径。不要单独移动 `report.md`，在 GitHub 上打开上面的报告链接即可。`tools/grade.sh` 保持官方三个测试的判定条件，仅将 QEMU 内核加载参数由 `-device loader,...` 改为 `-kernel bin/ucore.img`，以适配本次验证环境；课程统一环境可能使用不同的固件或启动参数。

## Git 提交

从本目录执行 `git add .` 即可；`.gitignore` 已排除 `obj/`、`bin/`、QEMU 运行日志和临时文件。
