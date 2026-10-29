#include "user.h"

extern char __stack_top[];

int syscall(int sysno, int arg0, int arg1, int arg2) {
    register int r0 __asm__("r0") = arg0;
    register int r1 __asm__("r1") = arg1;
    register int r2 __asm__("r2") = arg2;
    register int r3 __asm__("r3") = sysno;

    __asm__ __volatile__("svc #0"
                         : "=r"(r0)
                         : "r"(r0), "r"(r1), "r"(r2), "r"(r3)
                         : "memory");

    return r0;
}

void putchar(char ch) {
    syscall(SYS_PUTCHAR, ch, 0, 0);
}

int getchar(void) {
    return syscall(SYS_GETCHAR, 0, 0, 0);
}

int readfile(const char *filename, char *buf, int len) {
    return syscall(SYS_READFILE, (int) filename, (int) buf, len);
}

int writefile(const char *filename, const char *buf, int len) {
    return syscall(SYS_WRITEFILE, (int) filename, (int) buf, len);
}

__attribute__((noreturn)) void exit(void) {
    syscall(SYS_EXIT, 0, 0, 0);
    for (;;);
}

__attribute__((section(".text.start")))
__attribute__((naked))
void start(void) {
    __asm__ __volatile__(
        "ldr sp, =__stack_top\n"
        "bl main\n"
        "bl exit\n");
}
