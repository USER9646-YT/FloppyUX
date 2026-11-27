#include "ulib.h"

#define SYS_UMOUNT 22

static int sys1(int n, int a) {
    int r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "0"(n), "b"(a)
                      : "memory");
    return r;
}

int main(int argc, char **argv) {
    int status = 0;

    if (argc < 2) {
        puts("usage: umount DIR...\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        int r = sys1(SYS_UMOUNT, (int)argv[i]);
        if (r < 0) { eprint("umount", argv[i], r); status = 1; }
    }
    return status;
}