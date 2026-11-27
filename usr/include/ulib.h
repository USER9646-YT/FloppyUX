#ifndef ULIB_H
#define ULIB_H

#include <unistd.h>

static inline unsigned int ustrlen(const char *s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline void puts(const char *s) {
    write(1, s, ustrlen(s));
}

static inline void print_int(int v) {
    char buf[12];
    int i = 11;
    unsigned int u = v < 0 ? -(unsigned int)v : (unsigned int)v;

    buf[i] = '\0';
    do { buf[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) buf[--i] = '-';
    puts(buf + i);
}

static inline const char *errstr(int e) {
    switch (-e) {
    case 1:  return "operation not permitted";
    case 2:  return "no such file or directory";
    case 3:  return "no such process";
    case 4:  return "interrupted";
    case 13: return "permission denied";
    case 16: return "device or resource busy";
    case 17: return "file exists";
    case 18: return "cross-device link";
    case 20: return "not a directory";
    case 21: return "is a directory";
    case 22: return "invalid argument";
    case 24: return "too many open files";
    case 27: return "file too large";
    case 28: return "no space left on device";
    case 30: return "read-only file system";
    case 32: return "broken pipe";
    case 36: return "name too long";
    case 39: return "directory not empty";
    default: return "error";
    }
}

// prints "prog: what: message"
static inline void eprint(const char *prog, const char *what, int e) {
    puts(prog);
    puts(": ");
    puts(what);
    puts(": ");
    puts(errstr(e));
    puts("\n");
}

#endif