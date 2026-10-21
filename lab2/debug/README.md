# 双 GDB 调试脚本

`dual_debug.sh` 用于分支任务“GDB 调试 QEMU 页表查询过程”。

## 使用

先准备带调试符号的 QEMU 4.1.1：

```bash
cd ~/qemu-debug
wget https://download.qemu.org/qemu-4.1.1.tar.xz
tar xJf qemu-4.1.1.tar.xz
cd qemu-4.1.1
./configure --target-list=riscv64-softmmu --enable-debug --disable-werror --python=python3
make -j$(nproc)
```

然后从 `lab2` 目录运行：

```bash
QEMU_DEBUG=/root/qemu-debug/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64   bash debug/dual_debug.sh
```

脚本会启动 QEMU、附加宿主 GDB、连接 RISC-V GDB，并打印从 TLB miss 到页表遍历再到物理地址的完整过程。

详细分析见 [`../report/debug.md`](../report/debug.md)。
