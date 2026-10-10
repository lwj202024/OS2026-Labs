# 实验1 提示词记录

**实验者**：王海同
**日期**：2026-10-08

## 提示词记录

1. **WSL 环境确认**：我已经在 WSL2 里配好了 VS Code，想确认一下实验需要的 `riscv64-unknown-elf-gcc`、`qemu-system-riscv64` 和 `make` 是否都装好了，用 `which` 检查了前两个，还需要检查哪些？

2. **Makefile 调试**：运行 `make qemu` 后，OpenSBI 正常打印，但内核没有输出 `(THU.CST) os is loading ...`，GDB 显示 `Domain0 Next Address` 是 `0x0`。我猜测是 QEMU 加载内核的方式有问题，请帮我分析一下 Makefile 里 `-device loader` 和 `-kernel` 的区别，以及为什么 OpenSBI v1.8 不会自动跳到 `0x80200000`。

3. **练习1的确认**：我看 `entry.S` 里的 `la sp, bootstacktop`，知道是设置栈指针，`tail kern_init` 是跳到 C 函数。但我不太明白：为什么内核栈要放在 `.data` 段而不是 `.bss` 段？

4. **地址的困惑**：用 GDB 调试时，我看到 PC 从 `0x1000` 变到 `0x80000000`，再变到 `0x80200000`。这三个地址分别是什么？是固定的吗？为什么内核一定要加载到 `0x80200000`，换个地址不行吗？
