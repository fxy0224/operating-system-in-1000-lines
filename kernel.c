#include "kernel.h"
#include "common.h"

extern char __kernel_base[];
extern char __stack_top[];
extern char __bss[], __bss_end[];
extern char __free_ram[], __free_ram_end[];
extern char _binary_shell_bin_start[], _binary_shell_bin_size[];

struct process procs[PROCS_MAX];
struct process *current_proc;
struct process *idle_proc;

paddr_t alloc_pages(uint32_t n) {  // 从可用内存起始位置 _free_ram 分配连续 n 个物理页。
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end)
        PANIC("out of memory");

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

/*
 * map_page: 用于在页表中映射虚拟地址到物理地址。
 * table1 指向一级页表的指针
 * vaddr 要映射的虚拟地址
 * paddr 对应的物理地址
 * flags 读写权限标志
 */
void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE))   // 检查虚拟地址是否对齐
        PANIC("unaligned vaddr %x", vaddr);

    if (!is_aligned(paddr, PAGE_SIZE))   // 检查物理地址是否对齐
        PANIC("unaligned paddr %x", paddr);

    uint32_t vpn1 = (vaddr >> 22) & 0x3ff;  // 从虚拟地址高位得到 VPN1（一级虚拟页号）
    if ((table1[vpn1] & PAGE_V) == 0) {     // 检查一级页表中能否找到有效的二级页表
        uint32_t pt_paddr = alloc_pages(1); // 找不到，那么分配一页作为二级页表。
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V; // 更新一级页表 VPN1，记录二级页表的地址
    }

    uint32_t vpn0 = (vaddr >> 12) & 0x3ff;  // 从虚拟地址找到 VPN0（二级虚拟页号）
    uint32_t *table0 = (uint32_t *) ((table1[vpn1] >> 10) * PAGE_SIZE); // 从一级页表中得到二级页表的地址
    table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;        // 将物理地址赋值给二级页表的虚拟页号。
}

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;

    __asm__ __volatile__("ecall"
                         : "=r"(a0), "=r"(a1)
                         : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                           "r"(a6), "r"(a7)
                         : "memory");
    return (struct sbiret){.error = a0, .value = a1};
}

struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
unsigned blk_capacity;

uint32_t virtio_reg_read32(unsigned offset) {
    return *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset));
}

uint64_t virtio_reg_read64(unsigned offset) {
    return *((volatile uint64_t *) (VIRTIO_BLK_PADDR + offset));
}

void virtio_reg_write32(unsigned offset, uint32_t value) {
    *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset)) = value;
}

void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
    virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

bool virtq_is_busy(struct virtio_virtq *vq) {
    return vq->last_used_index != *vq->used_index;
}

void virtq_kick(struct virtio_virtq *vq, int desc_index) {
    vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
    vq->avail.index++;
    __sync_synchronize();
    virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
    vq->last_used_index++;
}

struct virtio_virtq *virtq_init(unsigned index) {
    paddr_t virtq_paddr = alloc_pages(align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
    struct virtio_virtq *vq = (struct virtio_virtq *) virtq_paddr;
    vq->queue_index = index;
    vq->used_index = (volatile uint16_t *) &vq->used.index;
    virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);
    virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
    virtio_reg_write32(VIRTIO_REG_QUEUE_ALIGN, 0);
    virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr);
    return vq;
}

void virtio_blk_init(void) {
    if (virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976)
        PANIC("virtio: invalid magic value");
    if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1)
        PANIC("virtio: invalid version");
    if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK)
        PANIC("virtio: invalid device id");

    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FEAT_OK);
    blk_request_vq = virtq_init(0);
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

    blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;
    printf("virtio-blk: capacity is %d bytes\n", blk_capacity);

    blk_req_paddr = alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
    blk_req = (struct virtio_blk_req *) blk_req_paddr;
}

void read_write_disk(void *buf, unsigned sector, int is_write) {
    if (sector >= blk_capacity / SECTOR_SIZE) {
        printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
              sector, blk_capacity / SECTOR_SIZE);
        return;
    }

    blk_req->sector = sector;
    blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;

    if (is_write)
        memcpy(blk_req->data, buf, SECTOR_SIZE);

    struct virtio_virtq *vq = blk_request_vq;
    vq->descs[0].addr = blk_req_paddr;
    vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
    vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
    vq->descs[0].next = 1;

    vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
    vq->descs[1].len = SECTOR_SIZE;
    vq->descs[1].flags = VIRTQ_DESC_F_NEXT | (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    vq->descs[1].next = 2;

    vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
    vq->descs[2].len = sizeof(uint8_t);
    vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

    virtq_kick(vq, 0);
    while (virtq_is_busy(vq))
        ;

    if (blk_req->status != 0) {
        printf("virtio: warn: failed to read/write sector=%d status=%d\n",
               sector, blk_req->status);
        return;
    }

    if (!is_write)
        memcpy(buf, blk_req->data, SECTOR_SIZE);
}

struct file files[FILES_MAX];
uint8_t disk[DISK_MAX_SIZE];

int oct2int(char *oct, int len) {
    int dec = 0;
    for (int i = 0; i < len; i++) {
        if (oct[i] < '0' || oct[i] > '7')
            break;

        dec = dec * 8 + (oct[i] - '0');
    }
    return dec;
}

void fs_flush(void) {
    memset(disk, 0, sizeof(disk));
    unsigned off = 0;
    for (int file_i = 0; file_i < FILES_MAX; file_i++) {
        struct file *file = &files[file_i];
        if (!file->in_use)
            continue;

        struct tar_header *header = (struct tar_header *) &disk[off];
        memset(header, 0, sizeof(*header));
        strcpy(header->name, file->name);
        strcpy(header->mode, "000644");
        strcpy(header->magic, "ustar");
        strcpy(header->version, "00");
        header->type = '0';

        int filesz = file->size;
        for (int i = sizeof(header->size); i > 0; i--) {
            header->size[i - 1] = (filesz % 8) + '0';
            filesz /= 8;
        }

        int checksum = ' ' * sizeof(header->checksum);
        for (unsigned i = 0; i < sizeof(struct tar_header); i++)
            checksum += (unsigned char) disk[off + i];

        for (int i = 5; i >= 0; i--) {
            header->checksum[i] = (checksum % 8) + '0';
            checksum /= 8;
        }

        memcpy(header->data, file->data, file->size);
        off += align_up(sizeof(struct tar_header) + file->size, SECTOR_SIZE);
    }

    for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, true);

    printf("wrote %d bytes to disk\n", sizeof(disk));
}

void fs_init(void) {
    for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);

    unsigned off = 0;
    for (int i = 0; i < FILES_MAX; i++) {
        struct tar_header *header = (struct tar_header *) &disk[off];
        if (header->name[0] == '\0')
            break;

        if (strcmp(header->magic, "ustar") != 0)
            PANIC("invalid tar header: magic=\"%s\"", header->magic);

        int filesz = oct2int(header->size, sizeof(header->size));
        struct file *file = &files[i];
        file->in_use = true;
        strcpy(file->name, header->name);
        memcpy(file->data, header->data, filesz);
        file->size = filesz;
        printf("file: %s, size=%d\n", file->name, file->size);

        off += align_up(sizeof(struct tar_header) + filesz, SECTOR_SIZE);
    }
}

struct file *fs_lookup(const char *filename) { // 在文件系统中查找文件
    for (int i = 0; i < FILES_MAX; i++) {
        struct file *file = &files[i];
        if (!strcmp(file->name, filename))
            return file;
    }

    return NULL;
}

void putchar(char ch) {
    sbi_call(ch, 0, 0, 0, 0, 0, 0, 1 /* Console Putchar */);
}

long getchar(void) {
    struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
    return ret.error;
}

__attribute__((naked))
__attribute__((aligned(4)))  // 该函数4字节对齐
void kernel_entry(void) {    // 函数功能：在内核栈中保存寄存器状态，然后执行 handle_trap 进行异常处理，最后将寄存器恢复，然后将控制权返回用户态，继续执行用户程序。
    __asm__ __volatile__(
        "csrrw sp, sscratch, sp\n"  // 这的 sp 存放用户态栈，sscratch 存放内核态栈。这句代码是 sp 和 sscratch 值互换。将用户态 sp 放在 sscratch，以便恢复到用户态时使用。
        "addi sp, sp, -4 * 31\n"    // 这里 sp 已经是是内核态栈了，这里是向下移动出31个寄存器的状态空间。（上一句代码中 sp 值 变成了 sscratch 寄存器中原来存着的内核态 sp）。
        "sw ra,  4 * 0(sp)\n"       // 依次在内核 sp 中保存寄存器状态。
        "sw gp,  4 * 1(sp)\n"
        "sw tp,  4 * 2(sp)\n"
        "sw t0,  4 * 3(sp)\n"
        "sw t1,  4 * 4(sp)\n"
        "sw t2,  4 * 5(sp)\n"
        "sw t3,  4 * 6(sp)\n"
        "sw t4,  4 * 7(sp)\n"
        "sw t5,  4 * 8(sp)\n"
        "sw t6,  4 * 9(sp)\n"
        "sw a0,  4 * 10(sp)\n"
        "sw a1,  4 * 11(sp)\n"
        "sw a2,  4 * 12(sp)\n"
        "sw a3,  4 * 13(sp)\n"
        "sw a4,  4 * 14(sp)\n"
        "sw a5,  4 * 15(sp)\n"
        "sw a6,  4 * 16(sp)\n"
        "sw a7,  4 * 17(sp)\n"
        "sw s0,  4 * 18(sp)\n"
        "sw s1,  4 * 19(sp)\n"
        "sw s2,  4 * 20(sp)\n"
        "sw s3,  4 * 21(sp)\n"
        "sw s4,  4 * 22(sp)\n"
        "sw s5,  4 * 23(sp)\n"
        "sw s6,  4 * 24(sp)\n"
        "sw s7,  4 * 25(sp)\n"
        "sw s8,  4 * 26(sp)\n"
        "sw s9,  4 * 27(sp)\n"
        "sw s10, 4 * 28(sp)\n"
        "sw s11, 4 * 29(sp)\n"

        "csrr a0, sscratch\n" 
        "sw a0,  4 * 30(sp)\n"    // 将用户态栈 sp 也放到内核态 sp 中

        "addi a0, sp, 4 * 31\n"   // a0 寄存器现在存的是内核态 sp 栈顶位置。
        "csrw sscratch, a0\n"     // ssctatch 现在是内核态 sp 栈顶，内核态栈的第 31 个位置存的是 用户态sp栈顶。

        "mv a0, sp\n"             // sp 现在又是内核态 sp 了
        "call handle_trap\n"      // 调用处理函数 handle_trap

        "lw ra,  4 * 0(sp)\n"     // 恢复上下文
        "lw gp,  4 * 1(sp)\n"
        "lw tp,  4 * 2(sp)\n"
        "lw t0,  4 * 3(sp)\n"
        "lw t1,  4 * 4(sp)\n"
        "lw t2,  4 * 5(sp)\n"
        "lw t3,  4 * 6(sp)\n"
        "lw t4,  4 * 7(sp)\n"
        "lw t5,  4 * 8(sp)\n"
        "lw t6,  4 * 9(sp)\n"
        "lw a0,  4 * 10(sp)\n"
        "lw a1,  4 * 11(sp)\n"
        "lw a2,  4 * 12(sp)\n"
        "lw a3,  4 * 13(sp)\n"
        "lw a4,  4 * 14(sp)\n"
        "lw a5,  4 * 15(sp)\n"
        "lw a6,  4 * 16(sp)\n"
        "lw a7,  4 * 17(sp)\n"
        "lw s0,  4 * 18(sp)\n"
        "lw s1,  4 * 19(sp)\n"
        "lw s2,  4 * 20(sp)\n"
        "lw s3,  4 * 21(sp)\n"
        "lw s4,  4 * 22(sp)\n"
        "lw s5,  4 * 23(sp)\n"
        "lw s6,  4 * 24(sp)\n"
        "lw s7,  4 * 25(sp)\n"
        "lw s8,  4 * 26(sp)\n"
        "lw s9,  4 * 27(sp)\n"
        "lw s10, 4 * 28(sp)\n"
        "lw s11, 4 * 29(sp)\n"
        "lw sp,  4 * 30(sp)\n"      // 4 *30（sp）是存放用户态栈顶的位置，这里sp又变成了用户态栈顶了，下一步就可以切回用户态了。
        "sret\n"                    // 返回用户模式
    );
}

__attribute__((naked)) void user_entry(void) { // 实现内核态到用户态的切换。
    __asm__ __volatile__(
        "csrw sepc, %[sepc]\n"        // sepc 保存发生异常时的程序计数器值，用于异常后恢复到用户态执行。
        "csrw sstatus, %[sstatus]\n"  // 设置状态寄存器的标志，控制终端和用户态的访问权限。
        "sret\n"                      // 从超级模式返回到用户模式。根据 sstatus 的设置恢复到用户态，跳转到 sepc 的位置。
        :
        : [sepc] "r" (USER_BASE),
          [sstatus] "r" (SSTATUS_SPIE | SSTATUS_SUM)
    );
}

/*
 * switch_context：为进程切换做准备
 * 将寄存器状态保存在 prev_sp（即将结束的进程的栈中）
 * 将 next_sp（接下来运行的进程的栈）中保存的寄存器状态放到寄存器里面，以便接下来切换到下一个进程。 
 */

__attribute__((naked)) void switch_context(uint32_t *prev_sp,    // 多任务上下文切换
                                           uint32_t *next_sp) {  // 参数是前后两个任务的栈指针
    __asm__ __volatile__(
        "addi sp, sp, -13 * 4\n"  // 在当前栈中分配空间，以存放寄存器状态。
        "sw ra,  0  * 4(sp)\n"    // 将当前寄存器状态存放在 sp 中保存，以便下一次切换回该任务时恢复寄存器。
        "sw s0,  1  * 4(sp)\n"
        "sw s1,  2  * 4(sp)\n"
        "sw s2,  3  * 4(sp)\n"
        "sw s3,  4  * 4(sp)\n"
        "sw s4,  5  * 4(sp)\n"
        "sw s5,  6  * 4(sp)\n"
        "sw s6,  7  * 4(sp)\n"
        "sw s7,  8  * 4(sp)\n"
        "sw s8,  9  * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"
        "sw sp, (a0)\n"           // a0 指的是 prev_sp，将当前存放了寄存器状态的 sp 地址保存在 prev_sp 中
        "lw sp, (a1)\n"           // sp 被 next_sp 赋值，sp 是下一个任务的栈指针了。
        "lw ra,  0  * 4(sp)\n"    // 将下一个任务的寄存器状态恢复到 sp 中
        "lw s0,  1  * 4(sp)\n"
        "lw s1,  2  * 4(sp)\n"
        "lw s2,  3  * 4(sp)\n"
        "lw s3,  4  * 4(sp)\n"
        "lw s4,  5  * 4(sp)\n"
        "lw s5,  6  * 4(sp)\n"
        "lw s6,  7  * 4(sp)\n"
        "lw s7,  8  * 4(sp)\n"
        "lw s8,  9  * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 13 * 4\n"  // 回到 next_sp 栈顶位置 
        "ret\n"                  // 返回指令，结束，控制权交给下一个任务
    );
}

/*
 * create_process: 创建新进程。 
 * 查找空闲的进程槽
 * 初始化进程栈和寄存器状态
 * 分配、设置进程的页表（分别建立内核程序、虚拟块设备、用户程序物理页面与虚拟页面的映射）
 * 设置进程的 id 和状态。
 */

struct process *create_process(const void *image, size_t image_size) { // 参数：image 是可执行代码的指针，image_size 代码的大小
    struct process *proc = NULL;
    int i;
    for (i = 0; i < PROCS_MAX; i++) {  // 从进程组里面找一个空闲的进程
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            break;
        }
    }

    if (!proc)
        PANIC("no free process slots");  // 如果没有空闲进程，则调用 PANIC

    uint32_t *sp = (uint32_t *) &proc->stack[sizeof(proc->stack)]; // sp 初始化为进程栈的栈顶
    *--sp = 0;                      // s11  依次设置的栈中的寄存器值
    *--sp = 0;                      // s10
    *--sp = 0;                      // s9
    *--sp = 0;                      // s8
    *--sp = 0;                      // s7
    *--sp = 0;                      // s6
    *--sp = 0;                      // s5
    *--sp = 0;                      // s4
    *--sp = 0;                      // s3
    *--sp = 0;                      // s2
    *--sp = 0;                      // s1
    *--sp = 0;                      // s0
    *--sp = (uint32_t) user_entry;  // ra （返回地址寄存器），ra 设置为user_entry，表示进程开始执行的入口点。user_entry 是内核态切换为用户态的入口处。

    uint32_t *page_table = (uint32_t *) alloc_pages(1);  // 分配一个页表，用来管理进程的虚拟地址空间映射（虚拟地址空间是连续的，与物理内存空间有映射关系，虚拟地址空间便于进程的安全性、隔离性、灵活性）

    // Kernel pages. 建立虚拟页面与内核态物理内存页面之间的映射（内核态没有用到虚拟页面）
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE)
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

    // virtio-blk  虚拟块设备的映射
    map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR, PAGE_R | PAGE_W);

    // User pages. 为用户程序分配物理页面并存放，然后进行虚拟页面地址映射。
    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page = alloc_pages(1);
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;
        memcpy((void *) page, image + off, copy_size);
        map_page(page_table, USER_BASE + off, page,
                 PAGE_U | PAGE_R | PAGE_W | PAGE_X);
    }

    proc->pid = i + 1;
    proc->state = PROC_RUNNABLE;
    proc->sp = (uint32_t) sp;
    proc->page_table = page_table;
    return proc;
}

/*
 * yield 实现进程调度。
 * 寻找下一个可运行的进程
 * 更新页表、栈指针
 * 切换上下文（切换寄存器状态）
 */
void yield(void) {
    struct process *next = idle_proc;
    for (int i = 0; i < PROCS_MAX; i++) {  // 遍历进程组，找到下一个正在等待运行的进程。
        struct process *proc = &procs[(current_proc->pid + i) % PROCS_MAX];
        if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
            next = proc;
            break;
        }
    }

    if (next == current_proc)
        return;

    struct process *prev = current_proc;
    current_proc = next;

    __asm__ __volatile__(                // 内联汇编更新页表和栈指针
        "sfence.vma\n"                   // 确保更新虚拟地址空间
        "csrw satp, %[satp]\n"           // 设置新的页表寄存器
        "sfence.vma\n"                   // 确保更新虚拟地址空间
        "csrw sscratch, %[sscratch]\n"   // 设置新的临时寄存器，指向下一个进程的栈
        :
        : [satp] "r" (SATP_SV32 | ((uint32_t) next->page_table / PAGE_SIZE)),
          [sscratch] "r" ((uint32_t) &next->stack[sizeof(next->stack)])
    );

    switch_context(&prev->sp, &next->sp);  // 寄存器状态保存和切花，为进程切换做准备。
}

/*
 * handle_syscall: 系统调用处理函数，用于处理用户程序发的系统调用请求。
 * 根据不同的类型，处理不同的系统调用
 * trap_frame 与用户程序进行参数传递、状态更新
*/

void handle_syscall(struct trap_frame *f) {   // 入参：保存了系统调用的参数和上下文信息（上下文信息其实就是寄存器状态）
    switch (f->a3) {         // 根据系统调用类型进行分支处理
        case SYS_PUTCHAR:
            putchar(f->a0);
            break;
        case SYS_GETCHAR:
            while (1) {
                long ch = getchar();
                if (ch >= 0) {
                    f->a0 = ch;
                    break;
                }

                yield();  // 进程切换
            }
            break;
        case SYS_EXIT:
            printf("process %d exited\n", current_proc->pid);
            current_proc->state = PROC_EXITED;
            yield();
            PANIC("unreachable");
        case SYS_READFILE:
        case SYS_WRITEFILE: {
            const char *filename = (const char *) f->a0;
            char *buf = (char *) f->a1;
            int len = f->a2;
            struct file *file = fs_lookup(filename);
            if (!file) {
                printf("file not found: %s\n", filename);
                f->a0 = -1;
                break;
            }

            if (len > (int) sizeof(file->data))
                len = file->size;

            if (f->a3 == SYS_WRITEFILE) {
                memcpy(file->data, buf, len);
                file->size = len;
                fs_flush();
            } else {
                memcpy(buf, file->data, len);
            }

            f->a0 = len;
            break;
        }
        default:
            PANIC("unexpected syscall a3=%x\n", f->a3);
    }
}

/*
 * handle_trap：处理来自于用户程序的异常，或者是系统调用
 *
 */
void handle_trap(struct trap_frame *f) {  // 入参是异常发生时的内存上下文
    uint32_t scause = READ_CSR(scause);   // 从控制和状态寄存器 scause 中获取走到这个函数的原因。
    uint32_t stval = READ_CSR(stval);     // 获取异常时的无效地址或者其他相关值。
    uint32_t user_pc = READ_CSR(sepc);    // 获取异常时的程序计数器。
    if (scause == SCAUSE_ECALL) {         // 如果是系统调用，那么处理系统调用，并且程序计数器往下走。以便系统调用处理完，程序继续往下走
        handle_syscall(f);
        user_pc += 4;
    } else {                              // 否则，调用 PNANIC 打印错误信息并终止程序。
        PANIC("unexpected trap scause=%x, stval=%x, sepc=%x\n", scause, stval, user_pc);
    }

    WRITE_CSR(sepc, user_pc);            // 更新程序计数器，以便异常处理完，继续执行
}

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss); // _bss是未初始化数据，将其清零（包括未初始化的全局变量、静态全局变量、静态局部变量）
    printf("\n\n");
    WRITE_CSR(stvec, (uint32_t) kernel_entry);             // stvec是中断寄存器，将kernel_entry的地址写入stvec，确保当中断发生时，kernel_entry响应和处理这些中断。
    virtio_blk_init();                                     // 初始化 Virtio 块设备驱动，通常用于管理虚拟磁盘或块设备的操作
    fs_init();                                             // 初始化文件系统

    idle_proc = create_process(NULL, 0);                   // 创建一个空闲进程
    idle_proc->pid = -1; // idle
    current_proc = idle_proc;

    create_process(_binary_shell_bin_start, (size_t) _binary_shell_bin_size);  // 创建新进程，加载 shell 程序
    yield();         // 进程切换，调度新创建的 shell 进程

    PANIC("switched to idle process");
}

__attribute__((section(".text.boot"))) // 将入口函数boot放在.text.boot中，放在启动时的位置。
__attribute__((naked)) // 不生成函数入口代码和函数出口代码。boot函数需要手动控制函数进入和退出
void boot(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n" // 内联汇编，将栈顶设置为kernel.ld中的栈顶位置
        "j kernel_main\n"   // 跳转到kernel_main函数
        :
        : [stack_top] "r" (__stack_top) // __stack_top 是kernel.ld中分配好的位置。
    );
}
