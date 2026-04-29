#!/bin/bash
# Boot the v6.2 + Morse + Drew + RoCC + backport kernel under QEMU,
# with the patched CBQRI device models, the buildroot rootfs from
# images/, and a virtio-9p host-share at cbqri_kern_mod/ for module
# iteration.
#
# Required: a patched riscv64 QEMU on $PATH or pointed at via $QEMU.
# See README.md for how to build it (tt-fustini/qemu, branch
# riscv-ssqosid-cbqri-rfc-v5).
#
# Inside the guest, after login as root:
#   mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt
#   sh /mnt/smoketest.sh
#
# Ctrl-A X to exit QEMU.

set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"

QEMU="${QEMU:-qemu-system-riscv64}"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "ERROR: $QEMU not found on PATH." >&2
    echo "Build the patched QEMU and either put it on PATH or set QEMU=/abs/path." >&2
    echo "See README.md for build instructions." >&2
    exit 1
fi

KERNEL="${KERNEL:-$ROOT/kernel/arch/riscv/boot/Image}"
if [ ! -f "$KERNEL" ]; then
    echo "ERROR: kernel image not built: $KERNEL" >&2
    echo "Build it: cd kernel && make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j Image modules" >&2
    exit 1
fi

exec "$QEMU" \
    -M virt -smp 8 -m 1G -nographic \
    -bios "$ROOT/images/fw_jump.bin" \
    -kernel "$KERNEL" \
    -dtb "$ROOT/dts/qemu-virt-cbqri.dtb" \
    -append "root=/dev/vda rw rootwait console=ttyS0" \
    -drive file="$ROOT/images/rootfs.ext2",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
    -fsdev local,id=hostshare,path="$ROOT/cbqri_kern_mod",security_model=none \
    -device virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare \
    -device riscv.cbqri.capacity,ncblks=12,mmio_base=0x04820000 \
    -device riscv.cbqri.capacity,ncblks=12,mmio_base=0x04821000 \
    -device riscv.cbqri.capacity,ncblks=16,mmio_base=0x0482B000 \
    -device riscv.cbqri.bandwidth,nbwblks=1024,mmio_base=0x04828000 \
    -device riscv.cbqri.bandwidth,nbwblks=1024,mmio_base=0x04829000 \
    -device riscv.cbqri.bandwidth,nbwblks=1024,mmio_base=0x0482a000 \
    "$@"
