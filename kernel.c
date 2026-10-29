#include "kernel.h"
#include "common.h"

extern char __kernel_base[];
extern char __stack_top[];
extern char __exception_stack_top[];
extern char __bss[], __bss_end[];
extern char __free_ram[], __free_ram_end[];
extern char _binary_shell_bin_start[], _binary_shell_bin_size[];

struct process procs[PROCS_MAX];
struct process *current_proc;
struct process *idle_proc;

paddr_t alloc_pages(uint32_t n) {
    static paddr_t next_paddr = (paddr_t) __free_ram;
    paddr_t paddr = next_paddr;
    next_paddr += n * PAGE_SIZE;

    if (next_paddr > (paddr_t) __free_ram_end)
        PANIC("out of memory");

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

uint32_t *alloc_l1_table(void) {
    // TTBR0 requires a 16KB-aligned L1 table. Over-allocate and align.
    paddr_t paddr = alloc_pages(L1_TABLE_SIZE * 2 / PAGE_SIZE);
    return (uint32_t *) align_up(paddr, L1_TABLE_SIZE);
}

void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE))
        PANIC("unaligned vaddr %x", vaddr);

    if (!is_aligned(paddr, PAGE_SIZE))
        PANIC("unaligned paddr %x", paddr);

    uint32_t index1 = vaddr >> 20;
    if ((table1[index1] & 0x3) == 0) {
        // An L2 table is 1KB (256 entries); a whole page is used for simplicity.
        uint32_t pt_paddr = alloc_pages(1);
        table1[index1] = pt_paddr | L1_TYPE_TABLE;
    }

    uint32_t index2 = (vaddr >> 12) & 0xff;
    uint32_t *table2 = (uint32_t *) (table1[index1] & ~0x3ff);
    uint32_t entry = paddr | L2_TYPE_SMALL;
    entry |= (flags & PAGE_U) ? L2_AP_ALL_RW : L2_AP_PRIV_RW;
    entry |= (flags & PAGE_DEVICE) ? L2_B : L2_TEX_NORMAL;
    if (!(flags & PAGE_X))
        entry |= L2_XN;
    table2[index2] = entry;
}

struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t blk_req_paddr;
uint64_t blk_capacity;

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
    virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);
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
    virtio_reg_write32(VIRTIO_REG_PAGE_SIZE, PAGE_SIZE);
    blk_request_vq = virtq_init(0);
    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER_OK);

    blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;
    printf("virtio-blk: capacity is %d bytes\n", (int)blk_capacity);

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

struct file *fs_lookup(const char *filename) {
    for (int i = 0; i < FILES_MAX; i++) {
        struct file *file = &files[i];
        if (!strcmp(file->name, filename))
            return file;
    }

    return NULL;
}

void putchar(char ch) {
    volatile uint32_t *uart = (volatile uint32_t *) UART_PADDR;
    while (uart[UART_FR / 4] & UART_FR_TXFF)
        ;
    uart[UART_DR / 4] = ch;
}

long getchar(void) {
    volatile uint32_t *uart = (volatile uint32_t *) UART_PADDR;
    if (uart[UART_FR / 4] & UART_FR_RXFE)
        return -1;
    return uart[UART_DR / 4] & 0xff;
}

__attribute__((naked))
__attribute__((aligned(32)))
void vectors(void) {
    __asm__ __volatile__(
        "b .\n"             // reset
        "b undef_entry\n"   // undefined instruction
        "b kernel_entry\n"  // supervisor call (svc)
        "b pabt_entry\n"    // prefetch abort
        "b dabt_entry\n"    // data abort
        "b .\n"             // reserved
        "b .\n"             // IRQ
        "b .\n"             // FIQ
    );
}

// Faults run on their own mode's stack (set up in boot) and never return.
__attribute__((naked)) void undef_entry(void) {
    __asm__ __volatile__("mov r0, #1\n sub r1, lr, #4\n b handle_fault\n");
}

__attribute__((naked)) void pabt_entry(void) {
    __asm__ __volatile__("mov r0, #3\n sub r1, lr, #4\n b handle_fault\n");
}

__attribute__((naked)) void dabt_entry(void) {
    __asm__ __volatile__("mov r0, #4\n sub r1, lr, #8\n b handle_fault\n");
}

// On svc, the CPU switches to SVC mode, whose banked sp already points to the
// top of the current process's kernel stack. lr_svc holds the return address
// and spsr_svc the user CPSR.
__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        "srsdb sp!, #0x13\n"       // push lr_svc (user pc) and spsr_svc
        "sub sp, sp, #4\n"         // pad
        "stmdb sp, {sp, lr}^\n"    // user sp and lr
        "sub sp, sp, #8\n"
        "stmdb sp!, {r0-r12}\n"

        "mov r0, sp\n"
        "bl handle_trap\n"

        "ldmia sp!, {r0-r12}\n"
        "ldmia sp, {sp, lr}^\n"
        "nop\n"
        "add sp, sp, #12\n"
        "rfeia sp!\n"              // pop pc and cpsr: back to user mode
    );
}

__attribute__((naked)) void user_entry(void) {
    __asm__ __volatile__(
        "mov r0, %[spsr]\n"
        "msr spsr_cxsf, r0\n"
        "mov lr, %[pc]\n"
        "movs pc, lr\n"            // jump to user mode
        :
        : [pc] "i" (USER_BASE),
          [spsr] "i" (CPSR_MODE_USR | 0xc0 /* IRQ and FIQ masked */)
    );
}

__attribute__((naked)) void switch_context(uint32_t *prev_sp,
                                           uint32_t *next_sp) {
    __asm__ __volatile__(
        "push {r4-r12, lr}\n"
        "str sp, [r0]\n"
        "ldr sp, [r1]\n"
        "pop {r4-r12, lr}\n"
        "bx lr\n"
    );
}

struct process *create_process(const void *image, size_t image_size) {
    struct process *proc = NULL;
    int i;
    for (i = 0; i < PROCS_MAX; i++) {
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            break;
        }
    }

    if (!proc)
        PANIC("no free process slots");

    uint32_t *sp = (uint32_t *) &proc->stack[sizeof(proc->stack)];
    *--sp = (uint32_t) user_entry;  // lr
    *--sp = 0;                      // r12
    *--sp = 0;                      // r11
    *--sp = 0;                      // r10
    *--sp = 0;                      // r9
    *--sp = 0;                      // r8
    *--sp = 0;                      // r7
    *--sp = 0;                      // r6
    *--sp = 0;                      // r5
    *--sp = 0;                      // r4

    uint32_t *page_table = alloc_l1_table();

    // Kernel pages.
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE)
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

    // UART and virtio-blk
    map_page(page_table, UART_PADDR, UART_PADDR, PAGE_R | PAGE_W | PAGE_DEVICE);
    map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR,
             PAGE_R | PAGE_W | PAGE_DEVICE);

    // User pages.
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

void yield(void) {
    struct process *next = idle_proc;
    for (int i = 0; i < PROCS_MAX; i++) {
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

    __asm__ __volatile__("dsb\n" ::: "memory");
    WRITE_CP15(c2, 0, c0, 0, (uint32_t) next->page_table); // TTBR0
    WRITE_CP15(c8, 0, c7, 0, 0);                            // TLBIALL
    __asm__ __volatile__("dsb\nisb\n" ::: "memory");
    // Enable the MMU (only changes something on the first switch).
    WRITE_CP15(c1, 0, c0, 0, READ_CP15(c1, 0, c0, 0) | SCTLR_M);
    __asm__ __volatile__("isb\n" ::: "memory");

    switch_context(&prev->sp, &next->sp);
}

void handle_syscall(struct trap_frame *f) {
    switch (f->r3) {
        case SYS_PUTCHAR:
            putchar(f->r0);
            break;
        case SYS_GETCHAR:
            while (1) {
                long ch = getchar();
                if (ch >= 0) {
                    f->r0 = ch;
                    break;
                }

                yield();
            }
            break;
        case SYS_EXIT:
            printf("process %d exited\n", current_proc->pid);
            current_proc->state = PROC_EXITED;
            yield();
            PANIC("unreachable");
        case SYS_READFILE:
        case SYS_WRITEFILE: {
            const char *filename = (const char *) f->r0;
            char *buf = (char *) f->r1;
            int len = f->r2;
            struct file *file = fs_lookup(filename);
            if (!file) {
                printf("file not found: %s\n", filename);
                f->r0 = -1;
                break;
            }

            if (len > (int) sizeof(file->data))
                len = file->size;

            if (f->r3 == SYS_WRITEFILE) {
                memcpy(file->data, buf, len);
                file->size = len;
                fs_flush();
            } else {
                memcpy(buf, file->data, len);
            }

            f->r0 = len;
            break;
        }
        default:
            PANIC("unexpected syscall r3=%x\n", f->r3);
    }
}

void handle_trap(struct trap_frame *f) {
    // Only svc is routed here; f->pc already points past the svc instruction.
    handle_syscall(f);
}

void handle_fault(uint32_t type, uint32_t pc) {
    PANIC("unexpected exception type=%d, pc=%x, dfsr=%x, dfar=%x, ifsr=%x, ifar=%x",
          type, pc, READ_CP15(c5, 0, c0, 0), READ_CP15(c6, 0, c0, 0),
          READ_CP15(c5, 0, c0, 1), READ_CP15(c6, 0, c0, 2));
}

void kernel_main(void) {
    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);
    printf("\n\n");
    WRITE_CP15(c12, 0, c0, 0, (uint32_t) vectors); // VBAR
    WRITE_CP15(c2, 0, c0, 2, 0);                    // TTBCR: always use TTBR0
    WRITE_CP15(c3, 0, c0, 0, 1);                    // DACR: domain 0 is client
    virtio_blk_init();
    fs_init();

    idle_proc = create_process(NULL, 0);
    idle_proc->pid = 0; // idle
    current_proc = idle_proc;

    create_process(_binary_shell_bin_start, (size_t) _binary_shell_bin_size);
    yield();

    PANIC("switched to idle process");
}

__attribute__((section(".text.boot")))
__attribute__((naked))
void boot(void) {
    __asm__ __volatile__(
        "cps #0x17\n"                      // abort mode
        "ldr sp, =__exception_stack_top\n"
        "cps #0x1b\n"                      // undefined mode
        "ldr sp, =__exception_stack_top\n"
        "cps #0x13\n"                      // supervisor mode
        "ldr sp, =__stack_top\n"
        "b kernel_main\n"
    );
}
