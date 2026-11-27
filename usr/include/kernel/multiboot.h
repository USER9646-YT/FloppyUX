#ifndef KERNEL_MULTIBOOT_H
#define KERNEL_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MBI_FLAG_MEM   (1u << 0)
#define MBI_FLAG_MMAP  (1u << 6)

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    // more fields follow in the real structure, which are not needed yet
};

struct multiboot_mmap_entry {
    uint32_t size;      // size of the rest of this entry NOT including this field
    uint64_t addr;
    uint64_t len;
    uint32_t type;      // 1 = usable RAM, everything else equals reserved
} __attribute__((packed));

#endif