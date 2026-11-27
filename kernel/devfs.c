#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "devfs.h"
#include "ramfs.h"
#include "vfs.h"
#include "tty.h"
#include "pit.h"
#include "io.h"

// ---------- /dev/null, /dev/zero ---------- //

static int null_read(struct vfs_node *n, uint32_t off, uint32_t len, void *buf) {
    (void)n; (void)off; (void)len; (void)buf;
    return 0;                                   // always end-of-file
}

static int sink_write(struct vfs_node *n, uint32_t off, uint32_t len, const void *buf) {
    (void)n; (void)off; (void)buf;
    return (int)len;                            // swallow everything
}

static int zero_read(struct vfs_node *n, uint32_t off, uint32_t len, void *buf) {
    (void)n; (void)off;
    memset(buf, 0, len);
    return (int)len;
}

// ---------- /dev/random, /dev/urandom ---------- //

// xorshift32: fine for games and tests, NOT cryptographically secure
static uint32_t rng_state = 0x2545F491u;

static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int random_read(struct vfs_node *n, uint32_t off, uint32_t len, void *buf) {
    (void)n; (void)off;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)(rng_next() >> 11);
    return (int)len;
}

static struct vfs_ops null_ops   = { .read = null_read,   .write = sink_write };
static struct vfs_ops zero_ops   = { .read = zero_read,   .write = sink_write };
static struct vfs_ops random_ops = { .read = random_read, .write = sink_write };

static struct vfs_node null_node = {
    .ino = 11, .type = VFS_CHAR, .refs = 1, .mode = S_IFCHR | 0666, .ops = &null_ops,
};
static struct vfs_node zero_node = {
    .ino = 12, .type = VFS_CHAR, .refs = 1, .mode = S_IFCHR | 0666, .ops = &zero_ops,
};
static struct vfs_node random_node = {
    .ino = 13, .type = VFS_CHAR, .refs = 1, .mode = S_IFCHR | 0666, .ops = &random_ops,
};

struct vfs_node *devfs_create(void) {
    struct vfs_node *root = ramfs_create_root();
    if (!root) return NULL;
    root->mode = S_IFDIR | 0755;                // only root may add device nodes

    // Seed from the RTC and the PIT. (No rdtsc: it needs a Pentium, and
    //   executing it on a 386/486 raises an invalid-opcode fault.)
    uint32_t rtc = 0;
    for (uint8_t reg = 0; reg <= 4; reg += 2) {         // seconds, minutes, hours
        outb(0x70, reg);
        rtc = (rtc << 8) | inb(0x71);
    }
    rng_state ^= rtc ^ (rtc << 17) ^ (pit_ticks() << 3);
    if (!rng_state) rng_state = 1;

    ramfs_add_node(root, "null",    &null_node);
    ramfs_add_node(root, "zero",    &zero_node);
    ramfs_add_node(root, "random",  &random_node);
    ramfs_add_node(root, "urandom", &random_node);
    ramfs_add_node(root, "console", tty_console());
    ramfs_add_node(root, "tty",     tty_console());
    return root;
}