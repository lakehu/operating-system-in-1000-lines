# ARMv7-A Port

This repository started as the RISC-V (riscv32) kernel from
*Operating System in 1,000 Lines*. The kernel sources in the repository root
have been ported to **32-bit ARM (ARMv7-A)** and run on QEMU's `virt` machine
with a Cortex-A15 CPU.

The last RISC-V version is commit `657882e` ("Build with clang 10 / lld 10 on
Ubuntu 20.04"). Use `git diff 657882e -- kernel.c` (or any other file) to see
exactly what the port changed.

The tutorial text under `website/` still describes RISC-V and has not been
updated.

## Quick start

```bash
./run.sh
```

`run.sh` builds the shell and kernel, packs `disk/*.txt` into `disk.tar`, and
boots QEMU. Exit QEMU with `Ctrl-A x`.

Expected output:

```text
virtio-blk: capacity is 10240 bytes
file: hello.txt, size=83
file: meow.txt, size=6
>
```

Shell commands: `hello`, `readfile`, `writefile`, `exit`.

## Toolchain

| Tool    | Version / binary                       | Notes                               |
|---------|----------------------------------------|-------------------------------------|
| C       | `clang` 10.0.0 (Ubuntu 20.04)          | `--target=armv7a-none-eabi`         |
| Linker  | `ld.lld` (lld 10)                      | Called directly, not through clang  |
| objcopy | `llvm-objcopy-10`                      |                                     |
| QEMU    | `qemu-system-arm` 7.1 (`~/usr/bin`)    | `-machine virt -cpu cortex-a15`     |

Compiler flags (from `run.sh`):

```text
-std=c11 -O2 -g3 -Wall -Wextra --target=armv7a-none-eabi -mcpu=cortex-a15
-marm -mfloat-abi=soft -mfpu=none -mno-unaligned-access
-fno-stack-protector -ffreestanding
```

- `-marm`: generate ARM (A32) code only. The trap and context-switch assembly
  assumes ARM state, not Thumb.
- `-mfloat-abi=soft -mfpu=none`: the kernel never enables the FPU, so any
  floating-point or NEON instruction would raise an undefined instruction
  exception.
- `-mno-unaligned-access`: the virtio and tar structures are `packed`. The
  compiler splits unaligned accesses instead of relying on hardware support.

Compiling and linking are separate steps (`clang -c`, then `ld.lld`). This was
needed for RISC-V on clang 10, where the driver ignored `-fuse-ld=lld`. For ARM
it is kept for consistency; it is not required.

The code still uses clang-only builtins (`__builtin_align_up`,
`__builtin_is_aligned`), so it does **not** build with GCC yet.

## Files changed by the port

| File        | Change                                                                |
|-------------|-----------------------------------------------------------------------|
| `kernel.c`  | Console, traps, user entry, context switch, paging, MMU, boot         |
| `kernel.h`  | ARM constants, `trap_frame`, CP15 access macros, virtio address       |
| `kernel.ld` | Load address `0x40200000`; extra stack for abort/undefined modes      |
| `user.c`    | `svc #0` syscall stub; `start()` stack setup in ARM assembly          |
| `run.sh`    | ARM compiler flags, `elf32-littlearm`, `qemu-system-arm`              |

Unchanged and shared with the RISC-V version: `shell.c`, `common.c`,
`common.h`, `user.h`, `user.ld`, `disk/`.

Removed: `opensbi-riscv32-generic-fw_dynamic.bin` (RISC-V firmware, not used by
the ARM build). It is still in commit `657882e`.

## Platform: QEMU `virt` (32-bit ARM)

| Address      | Device / region           | Used for                                 |
|--------------|---------------------------|------------------------------------------|
| `0x09000000` | PL011 UART                | Console (`putchar` / `getchar`)          |
| `0x0a000000` | virtio-mmio slot 0        | virtio-blk disk (`-bus virtio-mmio-bus.0`) |
| `0x40000000` | Start of RAM              |                                          |
| `0x40200000` | Kernel load address       | `kernel.ld`                              |

QEMU's `-kernel` option loads `kernel.elf` and jumps to `boot()` in SVC mode
with the MMU off. There is no firmware layer (OpenSBI on RISC-V).

The virtio-mmio devices on `virt` use the legacy interface (version 1), which
is what the existing driver already expects. Only the base address changed.

## Concept mapping: RISC-V → ARMv7-A

| RISC-V (S-mode)               | ARMv7-A (SVC mode)                     | Where                   |
|-------------------------------|----------------------------------------|-------------------------|
| OpenSBI → `boot()`            | QEMU `-kernel` → `boot()`              | `kernel.c: boot`        |
| SBI console putchar/getchar   | PL011 UART registers                   | `putchar`, `getchar`    |
| `ecall`                       | `svc #0`                               | `user.c: syscall`       |
| Syscall args `a0`–`a2`, `a3`  | `r0`–`r2`, number in `r3`              | `handle_syscall`        |
| `stvec`                       | `VBAR` + 8-entry vector table          | `vectors`, `kernel_main`|
| `sscratch` (kernel sp swap)   | Banked `sp_svc` (done by hardware)     | `kernel_entry`          |
| `sepc`                        | `lr_svc` (saved in `trap_frame.pc`)    | `kernel_entry`          |
| `sstatus`                     | `spsr_svc` (saved in `trap_frame.spsr`)| `kernel_entry`          |
| `scause` / `stval`            | Vector slot + `DFSR`/`DFAR`/`IFSR`/`IFAR` | `handle_fault`       |
| `sret`                        | `rfeia` (trap return), `movs pc, lr`   | `kernel_entry`, `user_entry` |
| `user_pc += 4` after `ecall`  | Not needed: `lr_svc` already points past `svc` | `handle_trap`   |
| `satp` + `sfence.vma`         | `TTBR0` + `TLBIALL` + `dsb`/`isb`      | `yield`                 |
| Sv32 page tables (2 × 1024)   | Short-descriptor (4096 L1 + 256 L2)    | `map_page`              |
| `READ_CSR` / `WRITE_CSR`      | `READ_CP15` / `WRITE_CP15`             | `kernel.h`              |

## Design details

### Boot (`boot`)

```text
cps #0x17   ; abort mode      → sp = __exception_stack_top
cps #0x1b   ; undefined mode  → sp = __exception_stack_top
cps #0x13   ; supervisor mode → sp = __stack_top
b kernel_main
```

ARM has a separate banked stack pointer for each exception mode. The abort and
undefined handlers only print a message and panic, so they share one 4KB stack
(`__exception_stack_top` in `kernel.ld`). IRQ and FIQ are not used and have no
stack.

`kernel_main` then sets:

- `VBAR` = `vectors` (exception vector table, 32-byte aligned)
- `TTBCR` = 0 (always translate through `TTBR0`)
- `DACR` = 1 (domain 0 is "client", so page permission bits are checked)

### Exception vectors (`vectors`)

| Offset | Exception             | Handler                        |
|--------|-----------------------|--------------------------------|
| `0x00` | Reset                 | `b .`                          |
| `0x04` | Undefined instruction | `undef_entry` → `handle_fault` |
| `0x08` | Supervisor call       | `kernel_entry` → `handle_trap` |
| `0x0c` | Prefetch abort        | `pabt_entry` → `handle_fault`  |
| `0x10` | Data abort            | `dabt_entry` → `handle_fault`  |
| `0x14` | Reserved              | `b .`                          |
| `0x18` | IRQ                   | `b .`                          |
| `0x1c` | FIQ                   | `b .`                          |

`handle_fault(type, pc)` prints the type (1 = undefined, 3 = prefetch abort,
4 = data abort), the faulting PC, and `DFSR`, `DFAR`, `IFSR`, `IFAR`, then
panics. As in the RISC-V version, faults are not recoverable.

### Syscall entry (`kernel_entry`)

When user code runs `svc #0`, the CPU switches to SVC mode. The banked `sp_svc`
already points to the top of the current process's kernel stack, so no
software stack swap is needed (RISC-V uses `sscratch` for this).

```text
srsdb sp!, #0x13        ; push lr_svc (user pc) and spsr_svc
sub   sp, sp, #4        ; pad
stmdb sp, {sp, lr}^     ; push user-mode sp and lr (^ = user bank)
sub   sp, sp, #8
stmdb sp!, {r0-r12}
mov   r0, sp
bl    handle_trap       ; handle_trap(struct trap_frame *)
ldmia sp!, {r0-r12}
ldmia sp, {sp, lr}^     ; restore user-mode sp and lr
nop                     ; required after ldm ^ before using banked regs
add   sp, sp, #12
rfeia sp!               ; pop pc and cpsr: return to user mode
```

Resulting `struct trap_frame` layout (low to high address, 18 words, 72 bytes):

```text
r0 r1 ... r12 | sp(user) lr(user) | pad | pc spsr
```

The `pad` word keeps the frame size a multiple of 8, so `sp_svc` stays 8-byte
aligned as the AAPCS requires when calling `handle_trap`.

`stm`/`ldm` with `^` accesses the **user-mode** `sp` and `lr`. Without `^`, SVC
mode would save its own banked copies instead.

### Entering user mode (`user_entry`)

A new process's first `switch_context` returns into `user_entry`:

```text
spsr_svc = USR mode | IRQ/FIQ masked
lr       = USER_BASE (0x1000000)
movs pc, lr             ; copy SPSR to CPSR and jump
```

User mode's `sp` is not set here. `start()` in `user.c` loads it from
`__stack_top` in `user.ld`, the same as the RISC-V version.

### Context switch (`switch_context`)

```text
push {r4-r12, lr}       ; callee-saved registers (+ r12 to keep 8-byte alignment)
str  sp, [r0]           ; prev->sp
ldr  sp, [r1]           ; next->sp
pop  {r4-r12, lr}
bx   lr
```

`create_process` builds the matching initial stack: `r4`–`r12` = 0 and
`lr` = `user_entry`. The process's kernel stack is declared
`__attribute__((aligned(8)))` so the initial `sp_svc` is 8-byte aligned.

When a process is in user mode, the top of its kernel stack is the value
`sp_svc` will have on the next `svc`. Because `kernel_entry` pops everything it
pushed before returning to user mode, this is always
`&proc->stack[sizeof(proc->stack)]`. No per-process "kernel stack top"
register write is needed in `yield()`.

### Paging (`alloc_l1_table`, `map_page`, `yield`)

ARMv7-A short-descriptor format with 4KB small pages:

```text
virtual address:  [31:20] L1 index (4096) | [19:12] L2 index (256) | [11:0] offset
```

- **L1 table:** 16KB, must be 16KB-aligned for `TTBR0`. `alloc_l1_table()`
  allocates 32KB and aligns the pointer up (this wastes up to 16KB per
  process).
- **L1 entry:** `L2 table address | 0x1` (page table type), domain 0.
- **L2 table:** 1KB. For simplicity, a whole 4KB page is allocated.
- **L2 entry:** `page address | 0x2` (small page) plus:

| Kernel flag          | L2 bits                                   |
|----------------------|-------------------------------------------|
| `PAGE_U` set         | `AP=11` (kernel RW, user RW)              |
| `PAGE_U` clear       | `AP=01` (kernel RW, user no access)       |
| `PAGE_X` clear       | `XN=1` (execute never)                    |
| `PAGE_DEVICE` set    | `TEX=000 C=0 B=1` (shareable device)      |
| `PAGE_DEVICE` clear  | `TEX=001 C=0 B=0` (normal, non-cacheable) |

`PAGE_R` and `PAGE_W` are still passed by callers but are not distinguished:
every mapped page is read/write. Read-only pages would need `AP[2]` (`APX`).

Each process's table maps:

- kernel memory `__kernel_base` … `__free_ram_end` (kernel only, executable)
- UART and virtio-blk registers (kernel only, device memory)
- the user image at `USER_BASE` (user, executable)

`yield()` switches address spaces:

```text
dsb
TTBR0 = next->page_table
TLBIALL
dsb; isb
SCTLR.M = 1             ; enable the MMU (only changes anything on the first switch)
isb
```

The kernel is identity-mapped in every table, so turning on the MMU while
running kernel code is safe.

## Testing

### Shell test

Run `./run.sh` and type:

| Command     | Expected output                                           |
|-------------|-----------------------------------------------------------|
| `hello`     | `Hello world from shell!`                                 |
| `readfile`  | `Can you see me? Ah, there you are! ... "Virtio Newbie!"` |
| `writefile` | `wrote 2560 bytes to disk`                                |
| `readfile`  | `Hello from shell!`                                       |
| `foo`       | `unknown command: foo`                                    |
| `exit`      | `process 2 exited`, then `PANIC: ... switched to idle process` |

The final PANIC after `exit` is expected and matches the RISC-V version: the
only user process has exited, so the scheduler falls back to the idle process.

`writefile` changes `disk.tar`. `run.sh` recreates it from `disk/*.txt` on
every run.

### Memory protection test

Replace the shell with a program that reads kernel memory from user mode:

```c
#include "user.h"

void main(void) {
    printf("user: reading kernel memory\n");
    volatile int *p = (int *) 0x40200000;
    printf("got %x\n", *p);
}
```

Expected: a data abort, not `got ...`:

```text
PANIC: kernel.c:...: unexpected exception type=4, pc=0100002c, dfsr=0000000f, dfar=40200000, ...
```

`DFSR = 0xf` is a permission fault on a small page; `DFAR` is the address that
was accessed.

### Debugging

- `run.sh` passes `-d unimp,guest_errors,int,cpu_reset -D qemu.log`. Every
  exception is logged to `qemu.log`.
- Only one QEMU can use `disk.tar` at a time. A second instance fails with
  `Failed to get "write" lock`.
- Disassemble with `llvm-objdump-10 -d kernel.elf`. The symbol map is in
  `kernel.map`.

## Known limitations

- **Caches are off.** Memory is mapped non-cacheable and `SCTLR.C`/`SCTLR.I`
  stay clear. This avoids cache maintenance but is slow.
- **No interrupts.** IRQ and FIQ stay masked; there is no timer, so scheduling
  is cooperative (same as the RISC-V version).
- **Faults panic.** Undefined instruction, prefetch abort and data abort are
  not recoverable.
- **All pages are read/write.** `PAGE_R`/`PAGE_W` are not enforced separately.
- **Memory use.** Each process uses 32KB for its L1 table and 4KB per L2 table.
- **clang only.** The clang builtins in `common.h` and the `naked` functions
  with operands need changes before GCC can build the code.
- **Tutorial text is still RISC-V.** `website/` has not been updated.

## Possible next steps

1. Replace `__builtin_align_up` / `__builtin_is_aligned` with plain macros and
   move entry code (`boot`, `vectors`, `kernel_entry`, `start`) into `.S`
   files, so the code also builds with `arm-none-eabi-gcc`.
2. Enable caches (`SCTLR.C`, `SCTLR.I`) with normal write-back memory
   attributes and add the needed cache maintenance.
3. Enforce read-only pages with `AP[2]`.
4. Add the generic timer and IRQ handling for preemptive scheduling.
5. Update the tutorial pages under `website/` for ARM.
