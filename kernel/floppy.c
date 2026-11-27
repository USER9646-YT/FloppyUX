#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "floppy.h"
#include "io.h"
#include "kprintf.h"
#include "pit.h"

// Floppy Disk Controller (82077AA) I/O ports
#define FDC_DOR  0x3F2   // Digital Output Register
#define FDC_MSR  0x3F4   // Main Status Register
#define FDC_FIFO 0x3F5   // Data FIFO
#define FDC_DIR  0x3F7   // Digital Input Register (read: bit 7 = disk changed)
#define FDC_CCR  0x3F7   // Configuration Control Register (write: data rate)

// MSR bits
#define MSR_CB   0x10    // Controller Busy
#define MSR_NDM  0x20    // Non-DMA execution phase
#define MSR_DIO  0x40    // Direction: 1 = FDC -> CPU, 0 = CPU -> FDC
#define MSR_RQM  0x80    // Data register ready for transfer

// FDC commands
#define CMD_SPECIFY         0x03
#define CMD_RECALIBRATE     0x07
#define CMD_SENSE_INTERRUPT 0x08
#define CMD_SEEK            0x0F
#define CMD_WRITE           0x45    // MFM Write Data
#define CMD_READ            0x46    // MFM Read Data

#define FDC_MAX_DRIVES 2
#define FDC_RES_BYTES  7
#define POLL_LIMIT_US  2000000u

// Low-memory scratch layout for 16-bit Real Mode BIOS int 0x13 fallback
#define RM_THUNK_ADDR  0x7000u
#define RM_STATE_ADDR  0x7200u
#define RM_BOUNCE_ADDR 0x8000u
#define RM_TMP_PT_ADDR 0x9000u

struct floppy_geom {
    uint8_t  cmos_type;
    uint8_t  ccr_rate;
    uint8_t  gap3;
    uint16_t cylinders;
    uint16_t heads;
    uint16_t spt;
    uint32_t total_sectors;
    const char *label;
};

static const struct floppy_geom geom_table[] = {
    { 1, 0x02, 0x2A, 40, 2,  9,  720, "360KB FLOPPY"  },
    { 2, 0x00, 0x1B, 80, 2, 15, 2400, "1.2MB FLOPPY"  },
    { 3, 0x02, 0x1B, 80, 2,  9, 1440, "720KB FLOPPY"  },
    { 4, 0x00, 0x1B, 80, 2, 18, 2880, "1.44MB FLOPPY" },
    { 5, 0x03, 0x38, 80, 2, 36, 5760, "2.88MB FLOPPY" },
};

#define GEOM_COUNT (sizeof(geom_table) / sizeof(*geom_table))

// 512-byte aligned buffer guarantees it never crosses a 64 KiB ISA DMA boundary
static uint8_t dma_buf[FLOPPY_SECTOR_SIZE] __attribute__((aligned(512)));

static struct floppy_drive drives[FLOPPY_MAX_DRIVES];
static const struct floppy_geom *drive_geom[FDC_MAX_DRIVES];
static int drive_is_bios[FDC_MAX_DRIVES] = { 0, 0 };
static uint8_t bios_dl[FDC_MAX_DRIVES] = { 0, 0 };
static int current_cyl[FDC_MAX_DRIVES] = { -1, -1 };
static int motor_running[FDC_MAX_DRIVES] = { 0, 0 };
static int line_works[FDC_MAX_DRIVES]    = { 0, 0 };   // disk-change line seen to clear on this drive
static int fdc_busy;                                   // a read/write is in progress: don't poll
static int fdc_attempts = 3;                           // retries per sector (probing uses 1)
static int fdc_quiet;                                  // suppress the "lba failed" message while probing

// =========================================================================
// 16-bit Real Mode BIOS int 0x13 Fallback (for USB Floppies / Legacy BIOS)
// =========================================================================

static struct floppy_geom bios_geom[FDC_MAX_DRIVES];

#define RM_GDT_COUNT 5

struct __attribute__((packed)) rm_state {
    uint32_t saved_esp;       // +0x00
    uint32_t saved_cr3;       // +0x04
    uint32_t saved_cr0;       // +0x08
    uint16_t saved_gdt_lim;   // +0x0C
    uint32_t saved_gdt_base;  // +0x0E
    uint16_t saved_idt_lim;   // +0x12
    uint32_t saved_idt_base;  // +0x14
    uint16_t tmp_gdt_lim;     // +0x18
    uint32_t tmp_gdt_base;    // +0x1A
    uint16_t bios_idt_lim;    // +0x1E
    uint32_t bios_idt_base;   // +0x20
    uint16_t saved_ds;        // +0x24
    uint16_t saved_es;        // +0x26
    uint16_t saved_fs;        // +0x28
    uint16_t saved_gs;        // +0x2A
    uint16_t saved_ss;        // +0x2C
    uint16_t bios_ax;         // +0x2E
    uint16_t bios_cx;         // +0x30
    uint16_t bios_dx;         // +0x32
    uint16_t bios_status;     // +0x34
    uint16_t out_cx;          // +0x36
    uint16_t out_dx;          // +0x38
    uint64_t tmp_gdt[RM_GDT_COUNT]; // +0x3A: 40 bytes
};

extern uint8_t bios_thunk_start;
extern uint8_t bios_thunk_jmp16_off;
extern uint8_t bios_thunk_pm16;
extern uint8_t bios_thunk_jmprm_off;
extern uint8_t bios_thunk_rm16;
extern uint8_t bios_thunk_jmp32_off;
extern uint8_t bios_thunk_pm32;
extern uint8_t bios_thunk_end;

__asm__(
    ".code32\n"
    ".globl bios_thunk_start\n"
    "bios_thunk_start:\n"
    "    pushal\n"
    "    movl %esp, 0x7200\n"
    "    movw %ds, 0x7224\n"
    "    movw %es, 0x7226\n"
    "    movw %fs, 0x7228\n"
    "    movw %gs, 0x722A\n"
    "    movw %ss, 0x722C\n"
    "    sgdt 0x720C\n"
    "    sidt 0x7212\n"
    "    movl %cr3, %eax\n"
    "    movl %eax, 0x7204\n"
    "    movl %cr0, %eax\n"
    "    movl %eax, 0x7208\n"
    "    lgdt 0x7218\n"
    "    .byte 0xEA\n"
    ".globl bios_thunk_jmp16_off\n"
    "bios_thunk_jmp16_off:\n"
    "    .long 0x00000000\n"
    "    .word 0x0008\n"

    ".code16\n"
    ".globl bios_thunk_pm16\n"
    "bios_thunk_pm16:\n"
    "    movw $0x0010, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    movw %ax, %ss\n"
    "    movl %cr0, %eax\n"
    "    andl $0x7FFFFFFF, %eax\n"
    "    movl %eax, %cr0\n"
    "    xorl %eax, %eax\n"
    "    movl %eax, %cr3\n"
    "    movl %cr0, %eax\n"
    "    andl $0xFFFFFFFE, %eax\n"
    "    movl %eax, %cr0\n"
    "    .byte 0xEA\n"
    ".globl bios_thunk_jmprm_off\n"
    "bios_thunk_jmprm_off:\n"
    "    .word 0x0000\n"
    "    .word 0x0000\n"

    ".globl bios_thunk_rm16\n"
    "bios_thunk_rm16:\n"
    "    xorw %ax, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    movw %ax, %ss\n"
    "    movw $0x7C00, %sp\n"
    "    lidt 0x721E\n"
    "    sti\n"
    "    movw $1, 0x7234\n"
    "    movw $3, %bp\n"
    "1:\n"
    "    xorw %di, %di\n"
    "    movw $0x7300, %si\n"
    "    movw 0x722E, %ax\n"
    "    movw 0x7230, %cx\n"
    "    movw 0x7232, %dx\n"
    "    movw $0x8000, %bx\n"
    "    int $0x13\n"
    "    jnc 2f\n"
    "    xorw %ax, %ax\n"
    "    movw 0x7232, %dx\n"
    "    int $0x13\n"
    "    decw %bp\n"
    "    jnz 1b\n"
    "    jmp 3f\n"
    "2:\n"
    "    movw %cx, 0x7236\n"
    "    movw %dx, 0x7238\n"
    "    movw $0, 0x7234\n"
    "3:\n"
    "    cli\n"
    "    lgdt 0x7218\n"
    "    movl %cr0, %eax\n"
    "    orl $0x00000001, %eax\n"
    "    movl %eax, %cr0\n"
    "    .byte 0x66, 0xEA\n"
    ".globl bios_thunk_jmp32_off\n"
    "bios_thunk_jmp32_off:\n"
    "    .long 0x00000000\n"
    "    .word 0x0018\n"

    ".code32\n"
    ".globl bios_thunk_pm32\n"
    "bios_thunk_pm32:\n"
    "    movw $0x0020, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    movw %ax, %ss\n"
    "    movl 0x7204, %eax\n"
    "    movl %eax, %cr3\n"
    "    movl 0x7208, %eax\n"
    "    movl %eax, %cr0\n"
    "    lgdt 0x720C\n"
    "    lidt 0x7212\n"
    "    movw 0x7224, %ds\n"
    "    movw 0x7226, %es\n"
    "    movw 0x7228, %fs\n"
    "    movw 0x722A, %gs\n"
    "    movw 0x722C, %ss\n"
    "    movl 0x7200, %esp\n"
    "    popal\n"
    "    ret\n"
    ".globl bios_thunk_end\n"
    "bios_thunk_end:\n"
);

static inline uintptr_t phys_to_virt(uint32_t phys) {
    return (uintptr_t)phys;
}

static void pic_set_offsets(uint8_t master_off, uint8_t slave_off,
                            uint8_t master_mask, uint8_t slave_mask) {
    outb(0x20, 0x11); outb(0x80, 0);
    outb(0xA0, 0x11); outb(0x80, 0);
    outb(0x21, master_off); outb(0x80, 0);
    outb(0xA1, slave_off); outb(0x80, 0);
    outb(0x21, 0x04); outb(0x80, 0);
    outb(0xA1, 0x02); outb(0x80, 0);
    outb(0x21, 0x01); outb(0x80, 0);
    outb(0xA1, 0x01); outb(0x80, 0);
    outb(0x21, master_mask);
    outb(0xA1, slave_mask);
}

#define RM_DAP_ADDR 0x7300u

struct __attribute__((packed)) bios_dap {
    uint8_t  size;
    uint8_t  reserved;
    uint16_t count;
    uint16_t buf_off;
    uint16_t buf_seg;
    uint64_t lba;
};

static int bios_transfer_sector(int idx, uint32_t lba, uint8_t *buf, int is_write) {
    uint32_t cr3_phys;
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_phys));

    uint8_t saved_mask1 = inb(0x21);
    uint8_t saved_mask2 = inb(0xA1);

    // Preserve existing PDE[0] kernel mappings (0x00100000+) while mapping 0x0000..0xFFFF
    uint32_t *pd = (uint32_t *)phys_to_virt(cr3_phys & ~0xFFFu);
    uint32_t saved_pde0 = *pd;
    uint32_t *orig_pt = (uint32_t *)phys_to_virt(saved_pde0 & ~0xFFFu);
    uint32_t *tmp_pt = (uint32_t *)phys_to_virt(RM_TMP_PT_ADDR);

    memcpy(tmp_pt, orig_pt, 1024u * sizeof(uint32_t));
    for (uint32_t i = 0; i < 16u; i++) {
        tmp_pt[i] = (i * 0x1000u) | 0x03u;
    }
    *pd = RM_TMP_PT_ADDR | 0x03u;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3_phys) : "memory");

    // Copy thunk code and initialize low-memory state
    size_t code_len = (size_t)(&bios_thunk_end - &bios_thunk_start);
    uint8_t *dst_code = (uint8_t *)(uintptr_t)RM_THUNK_ADDR;
    memcpy(dst_code, &bios_thunk_start, code_len);

    uint32_t pm16_addr = RM_THUNK_ADDR + (uint32_t)(&bios_thunk_pm16 - &bios_thunk_start);
    uint16_t rm16_addr = (uint16_t)(RM_THUNK_ADDR + (uint32_t)(&bios_thunk_rm16 - &bios_thunk_start));
    uint32_t pm32_addr = RM_THUNK_ADDR + (uint32_t)(&bios_thunk_pm32 - &bios_thunk_start);

    *(uint32_t *)(dst_code + (&bios_thunk_jmp16_off - &bios_thunk_start)) = pm16_addr;
    *(uint16_t *)(dst_code + (&bios_thunk_jmprm_off - &bios_thunk_start)) = rm16_addr;
    *(uint32_t *)(dst_code + (&bios_thunk_jmp32_off - &bios_thunk_start)) = pm32_addr;

    static const uint64_t gdt_entries[] = {
        0x0000000000000000ULL, // 0x00: Null
        0x00009A000000FFFFULL, // 0x08: 16-bit code
        0x000092000000FFFFULL, // 0x10: 16-bit data
        0x00CF9A000000FFFFULL, // 0x18: 32-bit code
        0x00CF92000000FFFFULL, // 0x20: 32-bit data
    };

    struct rm_state *st = (struct rm_state *)(uintptr_t)RM_STATE_ADDR;
    memset(st, 0, sizeof(*st));
    st->tmp_gdt_lim   = sizeof(gdt_entries) - 1;
    st->tmp_gdt_base  = RM_STATE_ADDR + offsetof(struct rm_state, tmp_gdt);
    st->bios_idt_lim  = 0x03FFu;
    st->bios_idt_base = 0;
    memcpy(st->tmp_gdt, gdt_entries, sizeof(gdt_entries));

    if (bios_dl[idx] >= 0x80u) {
        // Use INT 13h EDD LBA transfer (AH=0x42 read / AH=0x43 write) for USB/HDD drives
        struct bios_dap *dap = (struct bios_dap *)(uintptr_t)RM_DAP_ADDR;
        dap->size     = sizeof(*dap);
        dap->reserved = 0;
        dap->count    = 1;
        dap->buf_off  = (uint16_t)RM_BOUNCE_ADDR;
        dap->buf_seg  = 0x0000u;
        dap->lba      = (uint64_t)lba;

        st->bios_ax = is_write ? 0x4300u : 0x4200u;
        st->bios_cx = 0;
        st->bios_dx = bios_dl[idx];
    } else {
        // Use standard INT 13h CHS transfer (AH=0x02 read / AH=0x03 write) for legacy floppies
        const struct floppy_geom *g = drive_geom[idx];
        uint32_t spc = (uint32_t)g->spt * (uint32_t)g->heads;
        uint8_t cyl  = (uint8_t)(lba / spc);
        uint32_t rem = lba % spc;
        uint8_t head = (uint8_t)(rem / g->spt);
        uint8_t sect = (uint8_t)((rem % g->spt) + 1);

        st->bios_ax = (uint16_t)((is_write ? 0x0300u : 0x0200u) | 0x01u);
        st->bios_cx = (uint16_t)(((uint16_t)cyl << 8) | sect);
        st->bios_dx = (uint16_t)(((uint16_t)head << 8) | bios_dl[idx]);
    }
    st->bios_status = 1;

    uint8_t *bounce = (uint8_t *)(uintptr_t)RM_BOUNCE_ADDR;
    if (is_write) {
        memcpy(bounce, buf, FLOPPY_SECTOR_SIZE);
    }

    pic_set_offsets(0x08, 0x70, 0x00, 0x00);

    void (*thunk_fn)(void) = (void (*)(void))(uintptr_t)RM_THUNK_ADDR;
    thunk_fn();

    pic_set_offsets(0x20, 0x28, saved_mask1, saved_mask2);

    int ok = (st->bios_status == 0);
    if (ok && !is_write) {
        memcpy(buf, bounce, FLOPPY_SECTOR_SIZE);
    }

    *pd = saved_pde0;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3_phys) : "memory");
    if (eflags & 0x200u) {
        __asm__ volatile ("sti");
    }

    return ok ? 0 : -1;
}

#define DRIVE_ZERO 0
#define GEOM_144MB 3

static int bios_raw_call(uint16_t ax, uint16_t cx, uint16_t dx, uint16_t *out_cx, uint16_t *out_dx) {
    uint32_t cr3_phys;
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_phys));

    uint8_t saved_mask1 = inb(0x21);
    uint8_t saved_mask2 = inb(0xA1);

    uint32_t *pd = (uint32_t *)phys_to_virt(cr3_phys & ~0xFFFu);
    uint32_t saved_pde0 = *pd;
    uint32_t *orig_pt = (uint32_t *)phys_to_virt(saved_pde0 & ~0xFFFu);
    uint32_t *tmp_pt = (uint32_t *)phys_to_virt(RM_TMP_PT_ADDR);

    memcpy(tmp_pt, orig_pt, 1024u * sizeof(uint32_t));
    for (uint32_t i = 0; i < 16u; i++) {
        tmp_pt[i] = (i * 0x1000u) | 0x03u;
    }
    *pd = RM_TMP_PT_ADDR | 0x03u;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3_phys) : "memory");

    size_t code_len = (size_t)(&bios_thunk_end - &bios_thunk_start);
    uint8_t *dst_code = (uint8_t *)(uintptr_t)RM_THUNK_ADDR;
    memcpy(dst_code, &bios_thunk_start, code_len);

    uint32_t pm16_addr = RM_THUNK_ADDR + (uint32_t)(&bios_thunk_pm16 - &bios_thunk_start);
    uint16_t rm16_addr = (uint16_t)(RM_THUNK_ADDR + (uint32_t)(&bios_thunk_rm16 - &bios_thunk_start));
    uint32_t pm32_addr = RM_THUNK_ADDR + (uint32_t)(&bios_thunk_pm32 - &bios_thunk_start);

    *(uint32_t *)(dst_code + (&bios_thunk_jmp16_off - &bios_thunk_start)) = pm16_addr;
    *(uint16_t *)(dst_code + (&bios_thunk_jmprm_off - &bios_thunk_start)) = rm16_addr;
    *(uint32_t *)(dst_code + (&bios_thunk_jmp32_off - &bios_thunk_start)) = pm32_addr;

    static const uint64_t gdt_entries[] = {
        0x0000000000000000ULL,
        0x00009A000000FFFFULL,
        0x000092000000FFFFULL,
        0x00CF9A000000FFFFULL,
        0x00CF92000000FFFFULL,
    };

    struct rm_state *st = (struct rm_state *)(uintptr_t)RM_STATE_ADDR;
    memset(st, 0, sizeof(*st));
    st->tmp_gdt_lim   = sizeof(gdt_entries) - 1;
    st->tmp_gdt_base  = RM_STATE_ADDR + offsetof(struct rm_state, tmp_gdt);
    st->bios_idt_lim  = 0x03FFu;
    st->bios_idt_base = 0;
    memcpy(st->tmp_gdt, gdt_entries, sizeof(gdt_entries));

    st->bios_ax     = ax;
    st->bios_cx     = cx;
    st->bios_dx     = dx;
    st->bios_status = 1;

    pic_set_offsets(0x08, 0x70, 0x00, 0x00);

    void (*thunk_fn)(void) = (void (*)(void))(uintptr_t)RM_THUNK_ADDR;
    thunk_fn();

    pic_set_offsets(0x20, 0x28, saved_mask1, saved_mask2);

    int ok = (st->bios_status == 0);
    if (ok && out_cx) *out_cx = st->out_cx;
    if (ok && out_dx) *out_dx = st->out_dx;

    *pd = saved_pde0;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3_phys) : "memory");
    if (eflags & 0x200u) {
        __asm__ volatile ("sti");
    }

    return ok ? 0 : -1;
}

static int bios_identify(int idx) {
    if (idx != DRIVE_ZERO) return 0;

    struct floppy_drive *d = &drives[idx];
    memset(d, 0, sizeof(*d));

    bios_geom[idx] = geom_table[GEOM_144MB];
    drive_geom[idx] = &bios_geom[idx];
    drive_is_bios[idx] = 1;

    static const uint8_t candidate_dls[] = { 0x00, 0x80 };
    uint8_t probe[FLOPPY_SECTOR_SIZE];
    int detected = 0;

    for (size_t i = 0; i < sizeof(candidate_dls); i++) {
        uint8_t dl = candidate_dls[i];
        uint16_t out_cx = 0, out_dx = 0;

        bios_geom[idx].heads = 2;
        bios_geom[idx].spt   = 18;

        // Query BIOS drive parameters (AH=0x08) to match translated USB/HDD CHS geometry
        if (bios_raw_call(0x0800u, 0, dl, &out_cx, &out_dx) == 0) {
            uint16_t spt   = (uint16_t)(out_cx & 0x3Fu);
            uint16_t heads = (uint16_t)(((out_dx >> 8) & 0xFFu) + 1u);
            if (spt > 0 && heads > 0) {
                bios_geom[idx].spt   = spt;
                bios_geom[idx].heads = heads;
            }
        }

        bios_dl[idx] = dl;
        if (bios_transfer_sector(idx, 0, probe, 0) == 0) {
            detected = 1;
            break;
        }
    }

    if (!detected) {
        drive_geom[idx] = NULL;
        drive_is_bios[idx] = 0;
        return 0;
    }

    d->present = 1;
    d->media   = 1;
    d->sectors = bios_geom[idx].total_sectors;
    const char *name = "BIOS/USB FLOPPY";
    for (int i = 0; name[i] && i < 40; i++) {
        d->model[i] = name[i];
    }
    return 1;
}

// =========================================================================
// Native 82077AA Floppy Controller + ISA DMA Channel 2 Driver
// =========================================================================

static void floppy_delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) {
        outb(0x80, 0);
    }
}

static int floppy_poll_rqm(uint8_t *msr_out) {
    for (uint32_t i = 0; i < POLL_LIMIT_US; i++) {
        uint8_t msr = inb(FDC_MSR);
        if (msr & MSR_RQM) {
            if (msr_out) *msr_out = msr;
            return 0;
        }
        outb(0x80, 0);
    }
    return -2;
}

static int fdc_send_byte(uint8_t val) {
    uint8_t msr = 0;
    if (floppy_poll_rqm(&msr) != 0) return -2;
    if (msr & MSR_DIO) return -1;
    outb(FDC_FIFO, val);
    return 0;
}

static int fdc_read_byte(uint8_t *val) {
    uint8_t msr = 0;
    if (floppy_poll_rqm(&msr) != 0) return -2;
    if (!(msr & MSR_DIO)) return -1;
    *val = inb(FDC_FIFO);
    return 0;
}

static void fdc_dma_setup(int is_write) {
    uintptr_t virt = (uintptr_t)dma_buf;
    uint32_t phys = (virt >= 0xC0000000u) ? (uint32_t)(virt - 0xC0000000u) : (uint32_t)virt;
    uint16_t count = FLOPPY_SECTOR_SIZE - 1;

    outb(0x0A, 0x06);
    outb(0x0C, 0xFF);
    outb(0x04, (uint8_t)(phys & 0xFF));
    outb(0x04, (uint8_t)((phys >> 8) & 0xFF));
    outb(0x81, (uint8_t)((phys >> 16) & 0xFF));

    outb(0x0C, 0xFF);
    outb(0x05, (uint8_t)(count & 0xFF));
    outb(0x05, (uint8_t)((count >> 8) & 0xFF));

    outb(0x0B, is_write ? 0x4A : 0x46);
    outb(0x0A, 0x02);
}

static void fdc_motor_on(int idx) {
    uint8_t dor = (uint8_t)(0x0C | (0x10 << idx) | (idx & 0x03));
    outb(FDC_DOR, dor);

    // This DOR value turns every other drive's motor off
    for (int j = 0; j < FDC_MAX_DRIVES; j++)
        if (j != idx) motor_running[j] = 0;

    if (drive_geom[idx]) {
        outb(FDC_CCR, drive_geom[idx]->ccr_rate);
    }

    if (!motor_running[idx]) {
        floppy_delay_us(350000u);
        motor_running[idx] = 1;
    }
}

static int fdc_sense_interrupt(uint8_t *st0, uint8_t *cyl) {
    if (fdc_send_byte(CMD_SENSE_INTERRUPT) != 0) return -1;
    if (fdc_read_byte(st0) != 0) return -1;
    if (*st0 == 0x80) return -1;
    if (fdc_read_byte(cyl) != 0) return -1;
    return 0;
}

static int fdc_specify(void) {
    if (fdc_send_byte(CMD_SPECIFY) != 0) return -1;
    if (fdc_send_byte(0x8F) != 0) return -1;
    if (fdc_send_byte(0x10) != 0) return -1;
    return 0;
}

static int fdc_reset(void) {
    outb(FDC_DOR, 0x00);
    floppy_delay_us(20);
    outb(FDC_DOR, 0x0C);
    floppy_delay_us(1000);

    uint8_t st0, cyl;
    for (int i = 0; i < 4; i++) {
        for (uint32_t t = 0; t < 10000u; t++) {
            if (fdc_sense_interrupt(&st0, &cyl) == 0) break;
            outb(0x80, 0);
        }
    }

    for (int i = 0; i < FDC_MAX_DRIVES; i++) {
        current_cyl[i] = -1;
        motor_running[i] = 0;
    }
    return fdc_specify();
}

static int fdc_recalibrate(int idx) {
    fdc_motor_on(idx);
    if (fdc_send_byte(CMD_RECALIBRATE) != 0) return -1;
    if (fdc_send_byte((uint8_t)idx) != 0) return -1;

    uint8_t st0 = 0, cyl = 0;
    for (uint32_t i = 0; i < POLL_LIMIT_US; i++) {
        if (fdc_sense_interrupt(&st0, &cyl) == 0 && (st0 & 0x20)) {
            current_cyl[idx] = 0;
            return (st0 & 0x10) ? -1 : 0;
        }
        outb(0x80, 0);
    }
    return -2;
}

static int fdc_seek(int idx, uint8_t cyl, uint8_t head) {
    fdc_motor_on(idx);

    if (inb(FDC_DIR) & 0x80) {
        uint8_t step_cyl = (cyl == 0) ? 1 : 0;
        if (fdc_send_byte(CMD_SEEK) == 0 &&
            fdc_send_byte((uint8_t)((head << 2) | idx)) == 0 &&
            fdc_send_byte(step_cyl) == 0) {
            uint8_t tmp_st0, tmp_pcn;
            for (uint32_t i = 0; i < POLL_LIMIT_US; i++) {
                if (fdc_sense_interrupt(&tmp_st0, &tmp_pcn) == 0 && (tmp_st0 & 0x20)) {
                    current_cyl[idx] = (int)step_cyl;
                    break;
                }
                outb(0x80, 0);
            }
        }
    }

    if (current_cyl[idx] == (int)cyl) return 0;

    if (fdc_send_byte(CMD_SEEK) != 0) return -1;
    if (fdc_send_byte((uint8_t)((head << 2) | idx)) != 0) return -1;
    if (fdc_send_byte(cyl) != 0) return -1;

    uint8_t st0 = 0, pcn = 0;
    for (uint32_t i = 0; i < POLL_LIMIT_US; i++) {
        if (fdc_sense_interrupt(&st0, &pcn) == 0 && (st0 & 0x20)) {
            if ((st0 & 0xC0) != 0 || pcn != cyl) return -1;
            current_cyl[idx] = (int)cyl;
            floppy_delay_us(15000);
            return 0;
        }
        outb(0x80, 0);
    }
    return -2;
}

static const struct floppy_geom *lookup_cmos_geom(int idx) {
    outb(0x70, 0x10);
    uint8_t cmos = inb(0x71);
    uint8_t type = (idx == DRIVE_ZERO) ? (cmos >> 4) : (cmos & 0x0F);

    for (size_t i = 0; i < GEOM_COUNT; i++) {
        if (geom_table[i].cmos_type == type) {
            return &geom_table[i];
        }
    }
    return NULL;
}

static int fdc_transfer_sector(int idx, uint32_t lba, uint8_t *buf, int is_write);

// All motors off (the controller stays enabled)
static void fdc_motor_off(void) {
    outb(FDC_DOR, 0x0C);
    for (int i = 0; i < FDC_MAX_DRIVES; i++) motor_running[i] = 0;
}

// Select a drive so its disk-change line can be read; no spin-up wait
static void fdc_select(int idx) {
    outb(FDC_DOR, (uint8_t)(0x0C | (0x10 << idx) | (idx & 0x03)));
    for (int j = 0; j < FDC_MAX_DRIVES; j++)
        if (j != idx) motor_running[j] = 0;
    if (drive_geom[idx]) outb(FDC_CCR, drive_geom[idx]->ccr_rate);
    floppy_delay_us(20000);
}

// Read sector 0 with the drive's geometry (a 2.88MB drive falls back to 1.44MB)
static int floppy_read_probe(int idx) {
    const struct floppy_geom *g = drive_geom[idx];
    uint8_t probe[FLOPPY_SECTOR_SIZE];

    if (fdc_transfer_sector(idx, 0, probe, 0) == 0) return 0;
    if (g->cmos_type == 5) {
        drive_geom[idx] = &geom_table[GEOM_144MB];
        if (fdc_transfer_sector(idx, 0, probe, 0) == 0) return 0;
        drive_geom[idx] = g;
    }
    return -1;
}

// Is there a readable disk in this drive? Used at boot.
static void floppy_probe_media(int idx) {
    struct floppy_drive *d = &drives[idx];

    fdc_motor_on(idx);
    fdc_seek(idx, 0, 0);                        // a step clears the change line if a disk is in
    int line = inb(FDC_DIR) & 0x80;

    // Line still set: probably empty, so one quiet attempt only. Some old drives
    // never clear the line, which is why we read anyway instead of trusting it.
    fdc_attempts = line ? 1 : 3;
    fdc_quiet    = line ? 1 : 0;
    int ok = (floppy_read_probe(idx) == 0);
    fdc_attempts = 3;
    fdc_quiet    = 0;

    if (ok) {
        d->media   = 1;
        d->sectors = drive_geom[idx]->total_sectors;
        if (!line) line_works[idx] = 1;
    } else {
        d->media   = 0;
        d->sectors = 0;
    }
}

static int floppy_probe_drive(int idx) {
    if (idx < 0 || idx >= FDC_MAX_DRIVES) return 0;

    struct floppy_drive *d = &drives[idx];
    memset(d, 0, sizeof(*d));

    const struct floppy_geom *g = lookup_cmos_geom(idx);
    if (!g) return 0;

    drive_geom[idx]    = g;
    drive_is_bios[idx] = 0;
    line_works[idx]    = 0;
    if (inb(FDC_MSR) == 0xFF) { drive_geom[idx] = NULL; return 0; }
    if (fdc_recalibrate(idx) != 0) { drive_geom[idx] = NULL; return 0; }

    d->present = 1;
    for (int i = 0; g->label[i] && i < 40; i++) d->model[i] = g->label[i];

    floppy_probe_media(idx);
    return 1;
}

int floppy_init(void) {
    int found = 0;

    if (inb(FDC_MSR) != 0xFF && fdc_reset() == 0) {
        for (int i = 0; i < FDC_MAX_DRIVES; i++) {
            if (!floppy_probe_drive(i)) continue;
            found++;
            if (drives[i].media)
                kprintf("floppy%d: %s, %u sectors (%u KiB)\n", i, drives[i].model,
                        drives[i].sectors, drives[i].sectors / 2);
            else
                kprintf("floppy%d: %s drive, no disk\n", i, drives[i].model);
        }
        fdc_motor_off();
    }

    // Fallback to 16-bit Real Mode BIOS int 0x13 when no hardware FDC exists (e.g. USB floppy)
    if (found == 0 && bios_identify(DRIVE_ZERO)) {
        found++;
        kprintf("floppy0: %s, %u sectors (%u KiB)\n", drives[DRIVE_ZERO].model,
                drives[DRIVE_ZERO].sectors, drives[DRIVE_ZERO].sectors / 2);
    }

    return found;
}

// ---- disk change detection ---- //

// Re-check one drive. Returns 1 if a disk was inserted, removed or swapped.
static int floppy_poll_one(int idx) {
    struct floppy_drive *d = &drives[idx];
    int had = d->media;

    fdc_select(idx);
    int line = inb(FDC_DIR) & 0x80;

    // A drive holding a disk, with a working change line that is quiet: nothing happened.
    // (If the line never worked on this drive we can't tell, so leave it alone.)
    if (had && (!line_works[idx] || !line)) return 0;

    // Line set: the drive is empty, or its disk was removed/swapped. A step pulse
    // clears the line when a disk is present, and stepping needs no spin-up.
    motor_running[idx] = 1;                     // skip the 350 ms spin-up wait while stepping
    if (current_cyl[idx] < 0) fdc_recalibrate(idx);
    fdc_seek(idx, 0, 0);

    int now = 0;
    if (!(inb(FDC_DIR) & 0x80)) {               // line cleared: there is a disk
        line_works[idx]    = 1;
        motor_running[idx] = 0;
        fdc_motor_on(idx);                      // now really spin it up
        fdc_attempts = 1;
        fdc_quiet    = 1;
        now = (floppy_read_probe(idx) == 0);
        fdc_attempts = 3;
        fdc_quiet    = 0;
    }

    int changed = (now != had) || (had && now && line);
    d->media   = now;
    d->sectors = now ? drive_geom[idx]->total_sectors : 0;
    if (changed) {
        d->generation++;
        kprintf("floppy%d: disk %s\n", idx, !now ? "removed" : had ? "changed" : "inserted");
    }
    return changed;
}

// Check every drive for disk changes and switch idle motors off.
// Cheap to call often: it does nothing more than once per half second.
// Returns the number of drives whose disk changed.
int floppy_poll(void) {
    static int      polled_once;
    static uint32_t last_tick;

    if (fdc_busy) return 0;

    uint32_t hz  = pit_freq() ? pit_freq() : 100;
    uint32_t now = pit_ticks();
    if (polled_once && (uint32_t)(now - last_tick) < hz / 2) return 0;
    polled_once = 1;
    last_tick   = now;

    int changed = 0;
    fdc_busy = 1;
    for (int i = 0; i < FDC_MAX_DRIVES; i++)
        if (drives[i].present && !drive_is_bios[i])
            changed += floppy_poll_one(i);
    fdc_motor_off();
    fdc_busy = 0;
    return changed;
}

const struct floppy_drive *floppy_get(int idx) {
    if (idx < 0 || idx >= FLOPPY_MAX_DRIVES || !drives[idx].present) return NULL;
    return &drives[idx];
}

static int floppy_setup(int idx, uint32_t lba, uint8_t cmd) {
    const struct floppy_geom *g = drive_geom[idx];
    uint32_t spc = (uint32_t)g->spt * (uint32_t)g->heads;

    uint8_t cyl  = (uint8_t)(lba / spc);
    uint32_t rem = lba % spc;
    uint8_t head = (uint8_t)(rem / g->spt);
    uint8_t sect = (uint8_t)((rem % g->spt) + 1);

    if (fdc_send_byte(cmd) != 0) return -1;
    if (fdc_send_byte((uint8_t)((head << 2) | (idx & 0x03))) != 0) return -1;
    if (fdc_send_byte(cyl) != 0) return -1;
    if (fdc_send_byte(head) != 0) return -1;
    if (fdc_send_byte(sect) != 0) return -1;
    if (fdc_send_byte(0x02) != 0) return -1;
    if (fdc_send_byte(sect) != 0) return -1;
    if (fdc_send_byte(g->gap3) != 0) return -1;
    if (fdc_send_byte(0xFF) != 0) return -1;
    return 0;
}

static int floppy_check(int idx, uint32_t lba, uint32_t count) {
    if (idx < 0 || idx >= FLOPPY_MAX_DRIVES || !drives[idx].present || !drives[idx].media || !drive_geom[idx]) return -1;
    if (lba + count < lba) return -1;
    if (lba + count > drives[idx].sectors) return -1;
    return 0;
}

static int fdc_transfer_sector(int idx, uint32_t lba, uint8_t *buf, int is_write) {
    const struct floppy_geom *g = drive_geom[idx];
    uint32_t spc = (uint32_t)g->spt * (uint32_t)g->heads;
    uint8_t cyl  = (uint8_t)(lba / spc);
    uint8_t head = (uint8_t)((lba % spc) / g->spt);

    for (int attempt = 0; attempt < fdc_attempts; attempt++) {
        if (fdc_seek(idx, cyl, head) != 0) {
            fdc_recalibrate(idx);
            continue;
        }

        if (is_write) {
            memcpy(dma_buf, buf, FLOPPY_SECTOR_SIZE);
        }

        fdc_dma_setup(is_write);

        if (floppy_setup(idx, lba, is_write ? CMD_WRITE : CMD_READ) != 0) {
            fdc_reset();
            fdc_recalibrate(idx);
            continue;
        }

        uint8_t msr = 0;
        int poll_rc = floppy_poll_rqm(&msr);

        uint8_t res[FDC_RES_BYTES];
        memset(res, 0, sizeof(res));
        int res_err = 0;
        for (int r = 0; r < FDC_RES_BYTES; r++) {
            if (fdc_read_byte(&res[r]) != 0) {
                res_err = 1;
                break;
            }
        }

        uint8_t st0 = *res;
        uint8_t st1 = *(res + 1);
        uint8_t st2 = *(res + 2);

        if (poll_rc == 0 && !res_err && (st0 & 0xC0) == 0 && st1 == 0 && st2 == 0) {
            if (!is_write) {
                memcpy(buf, dma_buf, FLOPPY_SECTOR_SIZE);
            }
            return 0;
        }

        if (attempt == fdc_attempts - 1 && !fdc_quiet) {
            kprintf("fdc: lba %u failed (rc=%d res_err=%d msr=%02x st=%02x/%02x/%02x)\n",
                    lba, poll_rc, res_err, msr, st0, st1, st2);
        }

        fdc_reset();
        fdc_recalibrate(idx);
    }

    return -1;
}

int floppy_read(int idx, uint32_t lba, uint32_t count, void *buf) {
    if (floppy_check(idx, lba, count) != 0) return -1;

    uint8_t *p = (uint8_t *)buf;
    int r = 0;
    fdc_busy = 1;
    for (uint32_t s = 0; s < count && r == 0; s++) {
        r = drive_is_bios[idx]
            ? bios_transfer_sector(idx, lba + s, p + (s * FLOPPY_SECTOR_SIZE), 0)
            : fdc_transfer_sector(idx, lba + s, p + (s * FLOPPY_SECTOR_SIZE), 0);
    }
    fdc_busy = 0;
    return r;
}

int floppy_write(int idx, uint32_t lba, uint32_t count, const void *buf) {
    if (floppy_check(idx, lba, count) != 0) return -1;

    const uint8_t *p = (const uint8_t *)buf;
    int r = 0;
    fdc_busy = 1;
    for (uint32_t s = 0; s < count && r == 0; s++) {
        r = drive_is_bios[idx]
            ? bios_transfer_sector(idx, lba + s, (uint8_t *)(p + (s * FLOPPY_SECTOR_SIZE)), 1)
            : fdc_transfer_sector(idx, lba + s, (uint8_t *)(p + (s * FLOPPY_SECTOR_SIZE)), 1);
    }
    fdc_busy = 0;
    return r;
}
