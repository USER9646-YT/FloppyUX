#ifndef KERNEL_FLOPPY_H
#define KERNEL_FLOPPY_H

#include <stdint.h>

#define FLOPPY_SECTOR_SIZE 512
#define FLOPPY_MAX_DRIVES  4

struct floppy_drive {
    int      present;       // a drive is installed
    int      media;         // a readable disk is in it
    uint32_t sectors;       // 512-byte sectors on the current disk (0 = no disk)
    uint32_t generation;    // bumped every time a disk is inserted, removed or swapped
    char     model[41];
};

int  floppy_init(void);                                   // returns number of drives found (with or without a disk)
const struct floppy_drive *floppy_get(int idx);              // NULL if no such drive
int  floppy_poll(void);                                   // re-check all drives for disk changes; returns how many changed
int  floppy_read (int idx, uint32_t lba, uint32_t count, void *buf);         // 0 = ok
int  floppy_write(int idx, uint32_t lba, uint32_t count, const void *buf);  // 0 = ok

#endif