#!/bin/bash
set -xue

QEMU=qemu-system-riscv32
CC=clang
OBJCOPY=llvm-objcopy-10
LD=ld.lld

# clang 10 ignores -fuse-ld=lld for riscv32-unknown-elf and calls GNU ld, so
# compile with clang and link with ld.lld directly. lld 10 does not implement
# RISC-V linker relaxation, hence -mno-relax.
CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -mno-relax -fno-stack-protector -ffreestanding"

# Build the shell.
$CC $CFLAGS -c shell.c user.c common.c
$LD -Tuser.ld -Map=shell.map -o shell.elf shell.o user.o common.o
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
$OBJCOPY -Ibinary -Oelf32-littleriscv shell.bin shell.bin.o

# Build the kernel.
$CC $CFLAGS -c kernel.c common.c
$LD -Tkernel.ld -Map=kernel.map -o kernel.elf kernel.o common.o shell.bin.o

(cd disk && tar cf ../disk.tar --format=ustar --owner=0 --group=0 *.txt)

$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
    -d unimp,guest_errors,int,cpu_reset -D qemu.log \
    -drive id=drive0,file=disk.tar,format=raw,if=none \
    -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
    -kernel kernel.elf
