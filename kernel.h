#pragma once
#include "common.h"

#define PROCS_MAX 8
#define PROC_UNUSED   0
#define PROC_RUNNABLE 1
#define PROC_EXITED   2
#define CPSR_MODE_USR 0x10
#define CPSR_MODE_SVC 0x13
#define SCTLR_M       (1 << 0)
#define PAGE_R    (1 << 1)
#define PAGE_W    (1 << 2)
#define PAGE_X    (1 << 3)
#define PAGE_U    (1 << 4)
#define PAGE_DEVICE (1 << 5)
#define L1_TABLE_SIZE   (16 * 1024) // 4096 entries, 1MB each
#define L1_TYPE_TABLE   0x1
#define L2_TYPE_SMALL   0x2
#define L2_XN           (1 << 0)
#define L2_B            (1 << 2)
#define L2_AP_PRIV_RW   (1 << 4)    // AP[1:0]=01: kernel RW, user no access
#define L2_AP_ALL_RW    (3 << 4)    // AP[1:0]=11: kernel and user RW
#define L2_TEX_NORMAL   (1 << 6)    // TEX=001, C=0, B=0: normal non-cacheable
#define UART_PADDR      0x09000000  // PL011
#define UART_DR         0x00
#define UART_FR         0x18
#define UART_FR_RXFE    (1 << 4)
#define UART_FR_TXFF    (1 << 5)
#define USER_BASE 0x1000000
#define FILES_MAX   2
#define DISK_MAX_SIZE     align_up(sizeof(struct file) * FILES_MAX, SECTOR_SIZE)
#define SECTOR_SIZE       512
#define VIRTQ_ENTRY_NUM   16
#define VIRTIO_DEVICE_BLK 2
#define VIRTIO_BLK_PADDR  0x0a000000
#define VIRTIO_REG_MAGIC         0x00
#define VIRTIO_REG_VERSION       0x04
#define VIRTIO_REG_DEVICE_ID     0x08
#define VIRTIO_REG_PAGE_SIZE     0x28
#define VIRTIO_REG_QUEUE_SEL     0x30
#define VIRTIO_REG_QUEUE_NUM_MAX 0x34
#define VIRTIO_REG_QUEUE_NUM     0x38
#define VIRTIO_REG_QUEUE_PFN     0x40
#define VIRTIO_REG_QUEUE_NOTIFY  0x50
#define VIRTIO_REG_DEVICE_STATUS 0x70
#define VIRTIO_REG_DEVICE_CONFIG 0x100
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

struct process {
    int pid; // 0 if it's an idle process
    int state; // PROC_UNUSED, PROC_RUNNABLE, PROC_EXITED
    vaddr_t sp; // kernel stack pointer
    uint32_t *page_table; // points to first level page table
    uint8_t stack[8192] __attribute__((aligned(8))); // kernel stack
};

struct trap_frame {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;   // user sp
    uint32_t lr;   // user lr
    uint32_t pad;  // keeps the frame 8-byte aligned
    uint32_t pc;   // return address (lr_svc)
    uint32_t spsr;
} __attribute__((packed));

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t index;
    uint16_t ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t index;
    struct virtq_used_elem ring[VIRTQ_ENTRY_NUM];
} __attribute__((packed));

struct virtio_virtq {
    struct virtq_desc descs[VIRTQ_ENTRY_NUM];
    struct virtq_avail avail;
    struct virtq_used used __attribute__((aligned(PAGE_SIZE)));
    int queue_index;
    volatile uint16_t *used_index;
    uint16_t last_used_index;
} __attribute__((packed));

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
} __attribute__((packed));

struct tar_header {
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

#define READ_CP15(crn, op1, crm, op2)                                          \
    ({                                                                         \
        uint32_t __tmp;                                                        \
        __asm__ __volatile__("mrc p15, " #op1 ", %0, " #crn ", " #crm ", " #op2 \
                             : "=r"(__tmp));                                   \
        __tmp;                                                                 \
    })

#define WRITE_CP15(crn, op1, crm, op2, value)                                  \
    do {                                                                       \
        uint32_t __tmp = (value);                                              \
        __asm__ __volatile__("mcr p15, " #op1 ", %0, " #crn ", " #crm ", " #op2 \
                             ::"r"(__tmp));                                    \
    } while (0)

#define PANIC(fmt, ...)                                                        \
    do {                                                                       \
        printf("PANIC: %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        while (1) {}                                                           \
    } while (0)
