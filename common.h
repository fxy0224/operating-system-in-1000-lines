#pragma once

typedef int bool;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t size_t;
typedef uint32_t paddr_t;
typedef uint32_t vaddr_t;

#define true  1
#define false 0
#define NULL  ((void *) 0)
#define align_up(value, align)   __builtin_align_up(value, align) // 内存对齐，对齐为align的倍数
#define is_aligned(value, align) __builtin_is_aligned(value, align) // 判断是否内存对齐
#define offsetof(type, member)   __builtin_offsetof(type, member)  // 计算结构体成员member到struct开头的偏移量
#define va_list  __builtin_va_list   // 用来存放不定数量参数的列表
#define va_start __builtin_va_start  // 用于初始化参数列表
#define va_end   __builtin_va_end    // 清理参数列表
#define va_arg   __builtin_va_arg   // 返回下一个参数
#define PAGE_SIZE 4096
#define SYS_PUTCHAR 1
#define SYS_GETCHAR 2
#define SYS_EXIT    3
#define SYS_READFILE  4
#define SYS_WRITEFILE 5

void *memset(void *buf, char c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
char *strcpy(char *dst, const char *src);
int strcmp(const char *s1, const char *s2);
void printf(const char *fmt, ...);
