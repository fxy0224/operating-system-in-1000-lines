#pragma once
#include "common.h"

#define PROCS_MAX 8       // 最多 8 个线程同时运行
#define PROC_UNUSED   0   // 进程状态：未使用，可用
#define PROC_RUNNABLE 1   // 进程状态：可用，可以被调度运行，正在等待
#define PROC_EXITED   2   // 进程状态：已退出，进程已结束并释放内存
#define SATP_SV32 (1u << 31)
#define SSTATUS_SPIE (1 << 5)
#define SSTATUS_SUM  (1 << 18)
#define SCAUSE_ECALL 8
#define PAGE_V    (1 << 0)  // 页有效 （内存页的权限和状态）
#define PAGE_R    (1 << 1)  // 页可被读取
#define PAGE_W    (1 << 2)  // 页可被写入
#define PAGE_X    (1 << 3)  // 页可被执行
#define PAGE_U    (1 << 4)  // 页可被用户模式程序访问
#define USER_BASE 0x1000000
#define FILES_MAX   2
#define DISK_MAX_SIZE     align_up(sizeof(struct file) * FILES_MAX, SECTOR_SIZE)
#define SECTOR_SIZE       512
#define VIRTQ_ENTRY_NUM   16
#define VIRTIO_DEVICE_BLK 2
#define VIRTIO_BLK_PADDR  0x10001000   // 是虚拟化环境中块设备的物理地址，支持虚拟机与宿主机之间的高效通信和数据传输。通过使用该地址，虚拟机能够正确地访问和操作虚拟块存储设备。
#define VIRTIO_REG_MAGIC         0x00
#define VIRTIO_REG_VERSION       0x04
#define VIRTIO_REG_DEVICE_ID     0x08
#define VIRTIO_REG_QUEUE_SEL     0x30
#define VIRTIO_REG_QUEUE_NUM_MAX 0x34
#define VIRTIO_REG_QUEUE_NUM     0x38
#define VIRTIO_REG_QUEUE_ALIGN   0x3c
#define VIRTIO_REG_QUEUE_PFN     0x40
#define VIRTIO_REG_QUEUE_NOTIFY  0x50
#define VIRTIO_REG_DEVICE_STATUS 0x70
#define VIRTIO_REG_DEVICE_CONFIG 0x100
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEAT_OK   8
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

struct process {
    int pid; // -1 if it's an idle process 闲置进程的 pid 是 -1
    int state; // PROC_UNUSED, PROC_RUNNABLE, PROC_EXITED
    vaddr_t sp; // kernel stack pointer
    uint32_t *page_table; // points to first level page table
    uint8_t stack[8192]; // kernel stack 内核栈
};

struct sbiret { // 系统调用返回值，错误码和返回值。
    long error;
    long value;
};

struct trap_frame { // 保存所有寄存器状态，用于上下文切换，处理系统调用和中断
    uint32_t ra;
    uint32_t gp;
    uint32_t tp;
    uint32_t t0;
    uint32_t t1;
    uint32_t t2;
    uint32_t t3;
    uint32_t t4;
    uint32_t t5;
    uint32_t t6;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t a4;
    uint32_t a5;
    uint32_t a6;
    uint32_t a7;
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t s3;
    uint32_t s4;
    uint32_t s5;
    uint32_t s6;
    uint32_t s7;
    uint32_t s8;
    uint32_t s9;
    uint32_t s10;
    uint32_t s11;
    uint32_t sp;
} __attribute__((packed));

struct virtq_desc {  // virtIO设备描述符
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {  // 管理VirtIO设备的可用描述符， 用于设备通信
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

struct virtq_used_elem {  // 已经完成的IO操作，用于设备通知
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {  // 管理已完成的IO操作，用于设备通信
    uint16_t flags;
    uint16_t index;
    struct virtq_used_elem ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

struct virtio_virtq {  // 完整的VirtIO虚拟队列
    struct virtq_desc descs[VIRTQ_ENTRY_NUM];
    struct virtq_avail avail;
    struct virtq_used used __attribute__((aligned(PAGE_SIZE)));
    int queue_index;
    volatile uint16_t *used_index;
    uint16_t last_used_index;
} __attribute__((packed));

struct virtio_blk_req {  // 定义块设备请求
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
} __attribute__((packed));

struct tar_header { // tar 归档文件的文件头信息
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
    char data[];
} __attribute__((packed));

struct file {
    bool in_use;
    char name[100];
    char data[1024];
    size_t size;
};

#define READ_CSR(reg)                                                          \
    ({                                                                         \
        unsigned long __tmp;                                                   \
        __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp));                  \
        __tmp;                                                                 \
    })
// ({ ... }) 是 GCC 的扩展语法，允许在宏中执行多条语句并返回最后一条语句的值。这使得宏的使用更为灵活，类似于一个函数。
#define WRITE_CSR(reg, value)                                                  \
    do {                                                                       \
        uint32_t __tmp = (value);                                              \
        __asm__ __volatile__("csrw " #reg ", %0" ::"r"(__tmp));                \
    } while (0)
// do { ... } while (0) 结构
// 这种结构用于确保宏在使用时表现得像一个单独的语句。即使宏内包含多行代码，使用时也能像单行代码一样工作，避免在控制结构中（如 if）引入错误。
#define PANIC(fmt, ...)                                                        \
    do {                                                                       \
        printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        while (1) {}                                                           \
    } while (0)
// 标准化的 panic 处理，详细的错误信息输出。进入死循环防止系统继续执行，等待人工干预。
// 打印错误信息，包括源文件的文件名和行号。__FILE__ 和 __LINE__ 是预定义的宏，分别表示当前源文件的名称和行号。