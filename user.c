#include "user.h"

extern char __stack_top[];

// syscall 用于发起系统调用， sysno 是系统调用的编号，后面几个参数是传给系统调用的参数
int syscall(int sysno, int arg0, int arg1, int arg2) {
    register int a0 __asm__("a0") = arg0;
    register int a1 __asm__("a1") = arg1;
    register int a2 __asm__("a2") = arg2;
    register int a3 __asm__("a3") = sysno;

    __asm__ __volatile__("ecall"      // ecall 触发系统调用
                         : "=r"(a0)   // 将返回值存在 a0 中
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3) // r 表示指定寄存器
                         : "memory");  // 汇编代码可能会影响内存，告诉编译器不进行优化

    return a0;
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

__attribute__((noreturn)) void exit(void) { // __attribute__((noreturn)) 表示函数不会返回调用它的地方。
    syscall(SYS_EXIT, 0, 0, 0);
    for (;;); // 保证syscall之后不会执行别的代码，理论上上一行会退出，不会走到这个for无限循环，写这个循环是为了什么防止上面没有终止代码走下来。
}

__attribute__((section(".text.start"))) // start函数放在 .text.start段
__attribute__((naked))  // 裸函数
void start(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n" // 设置栈顶指针
        "call main\n"           // 调用main函数， 然后调用exit函数。main函数在 shell.c 中。
        "call exit\n" ::[stack_top] "r"(__stack_top));
}
