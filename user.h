#pragma once
#include "common.h"

/* 定义了用户程序和操作系统内核交互的基本接口：标准输入输出、文件读写、进程控制 */
struct sysret {  // 系统调用返回值结构，用于有多个返回值的系统调用
    int a0;
    int a1;
    int a2;
};

void putchar(char ch); // 用户程序的标准输出函数
int getchar(void);     // 用户程序的标准输入接口，返回读取的字符
int readfile(const char *filename, char *buf, int len);  // 文件读取系统调用，返回读取到的文件的字节数
int writefile(const char *filename, const char *buf, int len);  // 文件写入系统调用，返回实际写入的字节数 
__attribute__((noreturn)) void exit(void); // 进程退出系统调用
