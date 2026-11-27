// SPDX-License-Identifier: GPL-2.0-only

// diskd - FloppyUX floppy disk watcher
//
// Copyright (C) 2026 USER9646
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License version 2 as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

#include <fcntl.h>
#include <unistd.h>
#ifndef DISKD_NO_HOOK
#include <sys/wait.h>
#include "ulib.h"
#endif

#define STATUS_FILE "/proc/floppy"
#define HOOK_FILE   "/etc/diskd.hook"
#ifndef CONSOLE
#define CONSOLE     "/dev/console"
#endif
#define BUF_MAX     1024

// The userland libc has no string functions, so these are local. The attribute
// stops gcc from turning the loops back into memcpy/strlen calls.
#define NOLIB __attribute__((optimize("no-tree-loop-distribute-patterns")))

static NOLIB int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static NOLIB int str_same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static NOLIB void copy_bytes(char *dst, const char *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

static void put(int fd, const char *s) {
    int len = str_len(s);
    while (len > 0) {
        int n = (int)write(fd, s, (unsigned int)len);
        if (n <= 0) return;
        s   += n;
        len -= n;
    }
}

// Fill buf with the whole status text; returns its length, or -1 on error
static int read_status(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    int len = 0;
    for (;;) {
        int n = (int)read(fd, buf + len, (unsigned int)(cap - 1 - len));
        if (n <= 0) break;
        len += n;
        if (len >= cap - 1) break;
    }
    close(fd);
    buf[len] = '\0';
    return len;
}

#ifndef DISKD_NO_HOOK
static void run_hook(const char *status) {
    int fd = open(HOOK_FILE, O_RDONLY);         // is there a hook at all?
    if (fd < 0) return;
    close(fd);

    int pid = fork();
    if (pid == 0) {
        char *args[3] = { (char *)HOOK_FILE, (char *)status, 0 };
        execve(HOOK_FILE, args, 0);
        _exit(127);
    }
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
}
#endif

static unsigned parse_uint(const char *s, unsigned def) {
    unsigned v = 0;
    if (!s || !*s) return def;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return def;
        v = v * 10 + (unsigned)(*s - '0');
    }
    return v ? v : def;
}

int main(int argc, char **argv) {
    puts("diskd: Started\n");
    unsigned    interval = parse_uint(argc > 1 ? argv[1] : NULL, 3);
    const char *path     = argc > 2 ? argv[2] : STATUS_FILE;

    char last[BUF_MAX] = "";
    char now[BUF_MAX];
    int  have_last = 0;

    for (;;) {
        int len = read_status(path, now, sizeof(now));

        if (len >= 0 && (!have_last || !str_same(now, last))) {
            int con = open(CONSOLE, O_WRONLY);
            int out = con >= 0 ? con : 1;

            put(out, "diskd: floppy drives:\n");
            put(out, now);
            if (con >= 0) close(con);

#ifndef DISKD_NO_HOOK
            if (have_last) run_hook(now);       // not for the initial state
#endif
            copy_bytes(last, now, len + 1);
            have_last = 1;
        }
        sleep(interval);
    }
}
