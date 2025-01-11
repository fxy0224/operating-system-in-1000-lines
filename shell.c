#include "user.h"

/* 实现一个简单的命令行 shell 程序 
 * 这个 shell 实现了四个功能：
 * 1. hello：打印 hello 信息
 * 2. exit：退出 shell
 * 3. readfile：读取hello.txt文件内容
 * 4. writefile：往hello.txt文件写内容
*/

void main(void) {
    while (1) { // 无限循环处理用户输入
prompt:
        printf("> ");
        char cmdline[128];
        for (int i = 0;; i++) {
            char ch = getchar();
            putchar(ch); // 回显字符
            if (i == sizeof(cmdline) - 1) {
                printf("command line too long\n");
                goto prompt;
            } else if (ch == '\r') { // \r 表示回车符，命令结束， break
                printf("\n");
                cmdline[i] = '\0'; // 添加字符串结束符 \0
                break;
            } else {
                cmdline[i] = ch;
            }
        }

        if (strcmp(cmdline, "hello") == 0)
            printf("Hello world from shell!\n");
        else if (strcmp(cmdline, "exit") == 0)
            exit();
        else if (strcmp(cmdline, "readfile") == 0) {
            char buf[128];
            int len = readfile("hello.txt", buf, sizeof(buf));
            buf[len] = '\0';
            printf("%s\n", buf);
        }
        else if (strcmp(cmdline, "writefile") == 0)
            writefile("hello.txt", "Hello from shell!\n", 19);
        else
            printf("unknown command: %s\n", cmdline);
    }
}
