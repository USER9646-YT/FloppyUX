#include <string.h>
#include <stdint.h>

//memcpy
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

//memmove
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}

static int _is_delim(char c, const char *delim) {
    while (*delim) {
        if (c == *delim++) return 1;
    }
    return 0;
}

char *strtok(char *str, const char *delim) {
    static char *saved_str = NULL;

    if (str != NULL) {
        saved_str = str;
    }
    if (saved_str == NULL) {
        return NULL;
    }

    while (*saved_str && _is_delim(*saved_str, delim)) {
        saved_str++;
    }

    if (*saved_str == '\0') {
        saved_str = NULL;
        return NULL;
    }

    char *token_start = saved_str;

    while (*saved_str && !_is_delim(*saved_str, delim)) {
        saved_str++;
    }

    if (*saved_str != '\0') {
        *saved_str = '\0';
        saved_str++;
    } else {
        saved_str = NULL;
    }

    return token_start;
}
