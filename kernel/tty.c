#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "tty.h"
#include "keyboard.h"
#include "terminal.h"
#include "proc.h"
#include "vfs.h"

#define LINE_MAX 256

static char     line[LINE_MAX];     // line being edited
static uint32_t line_len;
static char     ready[LINE_MAX];    // completed line, being read
static uint32_t ready_len, ready_pos;
static int      echo = 1;
static int      raw;
static int      esc_state;          // cooked mode swallows escape sequences (arrow keys etc.)

void tty_set_echo(int on) {
    echo = (on != 0);
}

void tty_set_raw(int on) {
    raw = (on != 0);
    line_len = 0;
    ready_len = ready_pos = 0;
    esc_state = 0;
}

static int raw_read(char *buf, uint32_t len) {
    int c;

    while ((c = keyboard_poll()) < 0) {
        sched_sleep(P_SLEEP_TTY);
        if (current->killed) return -EINTR;
    }

    uint32_t n = 0;
    buf[n++] = (char)c;
    while (n < len && (c = keyboard_poll()) >= 0)
        buf[n++] = (char)c;
    return (int)n;
}

static int cooked_read(char *buf, uint32_t len) {
    while (ready_pos == ready_len) {
        int c = keyboard_poll();
        if (c < 0) {                        // nothing typed: sleep until the keyboard IRQ
            sched_sleep(P_SLEEP_TTY);
            if (current->killed) return -EINTR;
            continue;
        }

        if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; continue; }
        if (esc_state == 2) { if (c >= 0x40 && c <= 0x7E) esc_state = 0; continue; }
        if (c == 27) { esc_state = 1; continue; }

        if (c == 4) {                       // Ctrl-D: end of file on an empty line
            if (line_len == 0) return 0;
            continue;
        }

        if (c == '\b') {
            if (line_len > 0) {
                line_len--;
                if (echo) terminal_putc('\b');
            }
        } else if (c == '\n') {
            line[line_len++] = '\n';
            terminal_putc('\n');            // always shown, even with echo off
            memcpy(ready, line, line_len);
            ready_len = line_len;
            ready_pos = 0;
            line_len  = 0;
            memset(line, 0, sizeof(line));  // don't leave passwords lying around
        } else if (c >= 32 && c < 127 && line_len < LINE_MAX - 1) {
            line[line_len++] = (char)c;
            if (echo) terminal_putc((char)c);
        }
    }

    uint32_t n = ready_len - ready_pos;
    if (n > len) n = len;
    memcpy(buf, ready + ready_pos, n);
    ready_pos += n;

    if (ready_pos == ready_len)
        memset(ready, 0, sizeof(ready));
    return (int)n;
}

int tty_read(char *buf, uint32_t len) {
    if (len == 0) return 0;
    return raw ? raw_read(buf, len) : cooked_read(buf, len);
}

// ---------- the console as a VFS character device ---------- //

static int console_read(struct vfs_node *n, uint32_t off, uint32_t len, void *buf) {
    (void)n; (void)off;
    return tty_read(buf, len);
}

static int console_write(struct vfs_node *n, uint32_t off, uint32_t len, const void *buf) {
    (void)n; (void)off;
    terminal_write(buf, len);
    return (int)len;
}

static struct vfs_ops console_ops = {
    .read  = console_read,
    .write = console_write,
};

static struct vfs_node console_node = {
    .ino  = 1,
    .type = VFS_CHAR,
    .refs = 1,                          // static: never freed
    .mode = S_IFCHR | 0666,
    .ops  = &console_ops,
};

struct vfs_node *tty_console(void) {
    return &console_node;
}