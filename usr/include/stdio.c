#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static FILE _stdin_file  = { .fd = 0, .flags = 1, .error = 0, .eof = 0 };
static FILE _stdout_file = { .fd = 1, .flags = 2, .error = 0, .eof = 0 };
static FILE _stderr_file = { .fd = 2, .flags = 2, .error = 0, .eof = 0 };

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

static size_t _strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void _out_char(char *buf, size_t size, size_t *pos, char c) {
    if (buf && size > 0 && *pos < size - 1) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void _out_str(char *buf, size_t size, size_t *pos, const char *s, int width, char pad) {
    if (!s) s = "(null)";
    int len = (int)_strlen(s);
    while (width > len) {
        _out_char(buf, size, pos, pad);
        width--;
    }
    while (*s) {
        _out_char(buf, size, pos, *s++);
    }
}

static void _out_uint(char *buf, size_t size, size_t *pos, unsigned long val, int base, int upper, int width, char pad) {
    char tmp;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % (unsigned long)base];
            val /= (unsigned long)base;
        }
    }

    while (width > i) {
        _out_char(buf, size, pos, pad);
        width--;
    }
    while (i > 0) {
        _out_char(buf, size, pos, tmp[--i]);
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    size_t pos = 0;

    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            _out_char(str, size, &pos, *p);
            continue;
        }

        p++;
        char pad = ' ';
        if (*p == '0') {
            pad = '0';
            p++;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        int is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == 'l') p++; /* Treat %ll same as %l on 64-bit, or basic fallback */
        } else if (*p == 'z') {
            is_long = 1;
            p++;
        }

        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                _out_str(str, size, &pos, s, width, ' ');
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                _out_char(str, size, &pos, c);
                break;
            }
            case 'd':
            case 'i': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                unsigned long u;
                if (v < 0) {
                    _out_char(str, size, &pos, '-');
                    if (width > 0) width--;
                    u = (unsigned long)(-(v + 1)) + 1;
                } else {
                    u = (unsigned long)v;
                }
                _out_uint(str, size, &pos, u, 10, 0, width, pad);
                break;
            }
            case 'u': {
                unsigned long u = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
                _out_uint(str, size, &pos, u, 10, 0, width, pad);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long u = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
                _out_uint(str, size, &pos, u, 16, (*p == 'X'), width, pad);
                break;
            }
            case 'p': {
                unsigned long ptr = (unsigned long)(uintptr_t)va_arg(ap, void *);
                _out_str(str, size, &pos, "0x", 0, ' ');
                _out_uint(str, size, &pos, ptr, 16, 0, width, pad);
                break;
            }
            case '%': {
                _out_char(str, size, &pos, '%');
                break;
            }
            case '\0':
                goto done;
            default:
                _out_char(str, size, &pos, '%');
                _out_char(str, size, &pos, *p);
                break;
        }
    }

done:
    if (str && size > 0) {
        str[(pos < size) ? pos : (size - 1)] = '\0';
    }
    return (int)pos;
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, (size_t)-1, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsprintf(str, format, ap);
    va_end(ap);
    return ret;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[BUFSIZ];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len < 0) return -1;
    size_t to_write = (size_t)len < sizeof(buf) ? (size_t)len : (sizeof(buf) - 1);
    if (fwrite(buf, 1, to_write, stream) < to_write) {
        return -1;
    }
    return len;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stream, format, ap);
    va_end(ap);
    return ret;
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stdout, format, ap);
    va_end(ap);
    return ret;
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    if (write(stream->fd, &ch, 1) != 1) {
        stream->error = 1;
        return EOF;
    }
    return ch;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *stream) {
    size_t len = _strlen(s);
    if (fwrite(s, 1, len, stream) < len) return EOF;
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

int fgetc(FILE *stream) {
    unsigned char ch;
    ssize_t n = read(stream->fd, &ch, 1);
    if (n == 0) {
        stream->eof = 1;
        return EOF;
    }
    if (n < 0) {
        stream->error = 1;
        return EOF;
    }
    return ch;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int getchar(void) {
    return fgetc(stdin);
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total = size * nmemb;
    if (total == 0) return 0;
    ssize_t n = read(stream->fd, ptr, total);
    if (n <= 0) {
        if (n == 0) stream->eof = 1;
        else stream->error = 1;
        return 0;
    }
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total = size * nmemb;
    if (total == 0) return 0;
    ssize_t n = write(stream->fd, ptr, total);
    if (n < 0) {
        stream->error = 1;
        return 0;
    }
    return (size_t)n / size;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0; /* All writes go directly to write() unbuffered */
}

FILE *fopen(const char *pathname, const char *mode) {
    int flags = 0;
    if (mode[0] == 'r') {
        flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
    } else if (mode[0] == 'w') {
        flags = ((mode[1] == '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = ((mode[1] == '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    } else {
        return NULL;
    }

    int fd = open(pathname, flags, 0666);
    if (fd < 0) return NULL;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->flags = flags;
    f->error = 0;
    f->eof = 0;
    return f;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    int ret = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return (ret < 0) ? EOF : 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (lseek(stream->fd, (off_t)offset, whence) < 0) {
        stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    return (long)lseek(stream->fd, 0, SEEK_CUR);
}

void rewind(FILE *stream) {
    fseek(stream, 0, SEEK_SET);
    stream->error = 0;
}

int feof(FILE *stream) {
    return stream->eof;
}

int ferror(FILE *stream) {
    return stream->error;
}

void clearerr(FILE *stream) {
    stream->eof = 0;
    stream->error = 0;
}

void perror(const char *s) {
    if (s && *s) {
        fprintf(stderr, "%s: error %d\n", s, errno);
    } else {
        fprintf(stderr, "error %d\n", errno);
    }
}
