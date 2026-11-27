#include <stdint.h>
#include <stddef.h>
#include "multiboot.h"
#include "terminal.h"
#include "kprintf.h"
#include "gdt.h"
#include "idt.h"
#include "syscall.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "floppy.h"
#include "part.h"
#include "vfs.h"
#include "ramfs.h"
#include "devfs.h"
#include "procfs.h"
#include "ext2.h"
#include "proc.h"

static void halt(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

static void panic_init_notexist(void) {
    __asm__ volatile ("cli");
    terminal_setcolor(VGA_WHITE, VGA_RED);
    kprintf("\npanic: no valid init found\n");
    kprintf("System halted.\n");
    halt();
}

static inline void mark(int col, char c) {
    ((volatile uint16_t *)0xB8000)[70 + col] = 0x0F00 | (uint8_t)c;
}

void kernel_main(uint32_t magic, struct multiboot_info *mbi) {
    terminal_init();
    mark(0,'1');

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        kprintf("Not booted by a Multiboot loader! (magic=%x)\n", magic);
        halt();
    }

    terminal_setcolor(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("If you see this, FloppyUX is running! (duh!)\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    kprintf("magic=%x  mbi=%p  flags=%08x\n", magic, mbi, mbi->flags);
    if (mbi->flags & MBI_FLAG_MEM)
        kprintf("memory: %u KiB lower, %u KiB upper\n", mbi->mem_lower, mbi->mem_upper);

    gdt_init();
    mark(1,'2');
    kprintf("GDT loaded\n");
    idt_init();
    mark(2,'3');
    syscall_init();
    kprintf("IDT loaded\n");

    pmm_init(mbi);
    mark(3,'4');
    kprintf("pmm: %u free frames (%u MiB)\n", pmm_free_frames(), pmm_free_frames() / 256);
    paging_init();
    mark(4,'5');
    kprintf("paging enabled\n");
    kheap_init();
    mark(5,'6');

    pic_remap();                 // IRQs -> vectors 32-47, everything masked
    pit_init(100);
    keyboard_init();

    int ndrives = floppy_init();
    kprintf("floppy: %d drive(s) found\n", ndrives);

    // The root filesystem lives in the first Linux partition of the disk;
    // a disk without a partition table is treated as one bare ext2 filesystem.
    uint32_t root_lba = 0, root_sectors = 0;
    if (part_find_linux(0, &root_lba, &root_sectors) == 0)
        kprintf("root: partition at sector %u (%u MiB)\n", root_lba, root_sectors / 2048);
    else
        kprintf("root: no partition table, using the whole disk\n");

    vfs_init();
    struct vfs_node *root = ext2_mount(0, root_lba);
    if (!root) {
        kprintf("cannot mount root filesystem!\n");
        halt();
    }
    vfs_mount_type("/", root, "ext2");
    // vfs_mount_type("/tmp", ramfs_create_root(), "ramfs");   // scratch space in RAM
    // vfs_mount_type("/dev", devfs_create(), "devfs");        // device nodes
    // vfs_mount_type("/proc", procfs_create(), "proc");       // process and system info

    struct vfs_node *run = ramfs_create_root();  // init <-> telinit, runlevel: root only
    if (run) run->mode = 040755;
    vfs_mount_type("/run", run, "ramfs");

    kprintf("starting init...\n");
    if (proc_spawn_init("/sbin/init") != 0) {
        kprintf("cannot start /sbin/init!\n");
        panic_init_notexist();
    }
    proc_start();                                // never returns
}