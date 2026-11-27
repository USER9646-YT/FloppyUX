#include "ulib.h"

#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_MOUNT 21

static int sys3(int n, int a, int b, int c) {
    int r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "0"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

// no arguments: show the current mounts
static int list_mounts(void) {
    char buf[256];
    int fd = sys3(SYS_OPEN, (int)"/proc/mounts", 0, 0);

    if (fd < 0) { eprint("mount", "/proc/mounts", fd); return 1; }

    int n;
    while ((n = sys3(SYS_READ, fd, (int)buf, sizeof(buf))) > 0)
        sys3(SYS_WRITE, 1, (int)buf, n);

    sys3(SYS_CLOSE, fd, 0, 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return list_mounts();

    if (argc != 5 || !streq(argv[1], "-t")) {
        puts("usage: mount -t proc|devfs|ramfs|tmpfs SOURCE DIR\n");
        puts("       mount\n");
        return 1;
    }

    int r = sys3(SYS_MOUNT, (int)argv[3], (int)argv[4], (int)argv[2]);
    if (r < 0) { eprint("mount", argv[4], r); return 1; }
    return 0;
}