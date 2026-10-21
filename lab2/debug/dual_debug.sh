#!/bin/bash
# Dual-GDB demo for the lab2 branch task.
# Terminal 1 (this script) starts QEMU, attaches host GDB to the QEMU process,
# and uses the RISC-V GDB to drive the guest kernel.
#
# Usage:
#   QEMU_DEBUG=/path/to/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64 \
#     bash debug/dual_debug.sh
#
# If QEMU_DEBUG is omitted, the path built during the lab is used.

set -u

CODE=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU_DEBUG:-/root/qemu-debug/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64}
CROSS=${CROSS_PREFIX:-/opt/riscv-elf-toolchains/bin/riscv64-unknown-elf-}

if [ ! -x "$QEMU" ]; then
    echo "QEMU debug binary not found: $QEMU"
    echo "Set QEMU_DEBUG to your qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64"
    exit 1
fi

cd "$CODE" || exit 1
make >/dev/null 2>&1 || exit 1

EDATA=$(${CROSS}nm -n bin/kernel | awk '$3 == "edata" { print "0x" $1 }')
if [ -z "$EDATA" ]; then
    echo "cannot find edata symbol in bin/kernel"
    exit 1
fi
QEMU_SRC=$(cd "$(dirname "$QEMU")/.." && pwd)

pkill -f "$QEMU" 2>/dev/null || true
rm -f /tmp/lab2_host.gdb /tmp/lab2_guest.gdb /tmp/lab2_host.log /tmp/lab2_guest.log /tmp/lab2_qemu.log

"$QEMU" -machine virt -nographic -bios default -kernel bin/ucore.img -s -S > /tmp/lab2_qemu.log 2>&1 &
qpid=$!
sleep 2

cat > /tmp/lab2_host.gdb <<EOF
set pagination off
directory $QEMU_SRC
attach $qpid
handle SIGPIPE nostop noprint
break target/riscv/cpu_helper.c:238 if addr == $EDATA
break target/riscv/cpu_helper.c:242 if addr == $EDATA
break target/riscv/cpu_helper.c:244 if addr == $EDATA
break target/riscv/cpu_helper.c:252 if addr == $EDATA
break target/riscv/cpu_helper.c:254 if addr == $EDATA
break target/riscv/cpu_helper.c:256 if addr == $EDATA
break target/riscv/cpu_helper.c:337 if addr == $EDATA
break target/riscv/cpu_helper.c:349 if addr == $EDATA
continue
bt 6
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
continue
p i
p ptshift
p/x idx
continue
p/x pte_addr
continue
p/x pte
continue
p/x pte
p/x ppn
continue
p/x ppn
continue
p/x *physical
continue
p/x *physical
p/x *prot
detach
quit
EOF

cat > /tmp/lab2_guest.gdb <<'EOF'
set pagination off
target remote localhost:1234
hbreak kern_init
continue
continue
EOF

timeout 35 gdb -q -batch -x /tmp/lab2_host.gdb > /tmp/lab2_host.log 2>&1 &
hpid=$!
sleep 1
timeout 25 ${CROSS}gdb -q -batch -nx -x /tmp/lab2_guest.gdb bin/kernel > /tmp/lab2_guest.log 2>&1 || true
kill "$hpid" 2>/dev/null || true
kill "$qpid" 2>/dev/null || true
sleep 1
pkill -f "$QEMU" 2>/dev/null || true

echo "edata = $EDATA"
echo '=== HOST GDB (QEMU source) ==='
cat /tmp/lab2_host.log
echo '=== GUEST GDB (ucore) ==='
cat /tmp/lab2_guest.log
echo '=== QEMU SERIAL (tail) ==='
tail -15 /tmp/lab2_qemu.log
