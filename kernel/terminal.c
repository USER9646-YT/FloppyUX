#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "terminal.h"
#include "io.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define TAB_WIDTH 8

static size_t  row, col;
static size_t  saved_row, saved_col;
static uint8_t cur_fg = VGA_LIGHT_GREY;
static uint8_t cur_bg = VGA_BLACK;
static int     reverse;

// ansi escape sequence parser state
static int esc_state;               // 0 = none, 1 = after ESC, 2 = inside ESC [ ...
static int params[8];
static int nparams;
static int have_digits;
static int private_seq;

static uint8_t attr(void) {
    if (reverse) return (uint8_t)(((cur_fg & 7) << 4) | (cur_bg & 7));
    return (uint8_t)(((cur_bg & 7) << 4) | (cur_fg & 15));
}

static uint16_t cell(char c) {
    return (uint16_t)(uint8_t)c | ((uint16_t)attr() << 8);
}

static void update_cursor(void) {
    uint16_t pos = (uint16_t)(row * TERM_COLS + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void clear_cells(size_t from, size_t to) {       // cell indexes, [from, to)
    for (size_t i = from; i < to; i++) VGA_MEM[i] = cell(' ');
}

static void scroll(void) {
    for (size_t i = 0; i < (size_t)(TERM_ROWS - 1) * TERM_COLS; i++)
        VGA_MEM[i] = VGA_MEM[i + TERM_COLS];
    clear_cells((size_t)(TERM_ROWS - 1) * TERM_COLS, (size_t)TERM_ROWS * TERM_COLS);
}

static void newline(void) {
    col = 0;
    if (++row == TERM_ROWS) {
        scroll();
        row = TERM_ROWS - 1;
    }
}

static void put_plain(char c) {
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        col = 0;
        break;
    case '\t':
        col = (col + TAB_WIDTH) & ~(size_t)(TAB_WIDTH - 1);
        if (col >= TERM_COLS) newline();
        break;
    case '\b':                                  // the tty uses this to erase an echoed character
        if (col > 0) {
            col--;
            VGA_MEM[row * TERM_COLS + col] = cell(' ');
        }
        break;
    default:
        VGA_MEM[row * TERM_COLS + col] = cell(c);
        if (++col == TERM_COLS) newline();
        break;
    }
}

// ---------- ansi escape sequences (the subset editors need) ---------- //

static int param_or(int i, int def) {
    return (i < nparams && params[i] > 0) ? params[i] : def;
}

static int param_raw(int i) {
    return i < nparams ? params[i] : 0;
}

static void sgr(void) {
    static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

    if (nparams == 0) {
        cur_fg = VGA_LIGHT_GREY; cur_bg = VGA_BLACK; reverse = 0;
        return;
    }
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0)        { cur_fg = VGA_LIGHT_GREY; cur_bg = VGA_BLACK; reverse = 0; }
        else if (p == 1)   cur_fg |= 8;
        else if (p == 22)  cur_fg &= 7;
        else if (p == 7)   reverse = 1;
        else if (p == 27)  reverse = 0;
        else if (p >= 30 && p <= 37)   cur_fg = (uint8_t)(ansi2vga[p - 30] | (cur_fg & 8));
        else if (p == 39)  cur_fg = VGA_LIGHT_GREY;
        else if (p >= 40 && p <= 47)   cur_bg = ansi2vga[p - 40];
        else if (p == 49)  cur_bg = VGA_BLACK;
        else if (p >= 90 && p <= 97)   cur_fg = (uint8_t)(ansi2vga[p - 90] | 8);
        else if (p >= 100 && p <= 107) cur_bg = ansi2vga[p - 100];
    }
}

static void csi_dispatch(char f) {
    if (private_seq) return;                    // ESC [ ? ... : not supported, ignored

    size_t n = (size_t)param_or(0, 1);

    switch (f) {
    case 'A': row = row >= n ? row - n : 0; break;
    case 'B': row += n; if (row >= TERM_ROWS) row = TERM_ROWS - 1; break;
    case 'C': col += n; if (col >= TERM_COLS) col = TERM_COLS - 1; break;
    case 'D': col = col >= n ? col - n : 0; break;
    case 'H':
    case 'f': {
        size_t r = (size_t)param_or(0, 1) - 1;
        size_t c = (size_t)param_or(1, 1) - 1;
        row = r >= TERM_ROWS ? TERM_ROWS - 1 : r;
        col = c >= TERM_COLS ? TERM_COLS - 1 : c;
        break;
    }
    case 'J': {
        size_t cur = row * TERM_COLS + col;
        int m = param_raw(0);
        if (m == 0)      clear_cells(cur, (size_t)TERM_ROWS * TERM_COLS);
        else if (m == 1) clear_cells(0, cur + 1);
        else             clear_cells(0, (size_t)TERM_ROWS * TERM_COLS);
        break;
    }
    case 'K': {
        size_t start = row * TERM_COLS;
        int m = param_raw(0);
        if (m == 0)      clear_cells(start + col, start + TERM_COLS);
        else if (m == 1) clear_cells(start, start + col + 1);
        else             clear_cells(start, start + TERM_COLS);
        break;
    }
    case 'm': sgr(); break;
    case 's': saved_row = row; saved_col = col; break;
    case 'u': row = saved_row; col = saved_col; break;
    default: break;
    }
}

static void put_char(char c) {
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            nparams = 0;
            have_digits = 0;
            private_seq = 0;
            for (int i = 0; i < 8; i++) params[i] = 0;
        } else {
            esc_state = 0;                      // unsupported escape: drop it
        }
        return;
    }

    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (!have_digits) {
                if (nparams < 8) nparams++;
                have_digits = 1;
            }
            if (nparams > 0) params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (!have_digits && nparams < 8) nparams++;     // empty parameter
            have_digits = 0;
        } else if (c == '?') {
            private_seq = 1;
        } else if (c >= 0x40 && c <= 0x7E) {
            csi_dispatch(c);
            esc_state = 0;
        } else {
            esc_state = 0;
        }
        return;
    }

    if (c == 0x1B) {
        esc_state = 1;
        return;
    }
    put_plain(c);
}

// ---------- public interface ---------- //

void terminal_setcolor(enum vga_color fg, enum vga_color bg) {
    cur_fg = (uint8_t)fg;
    cur_bg = (uint8_t)bg;
    reverse = 0;
}

void terminal_clear(void) {
    clear_cells(0, (size_t)TERM_ROWS * TERM_COLS);
    row = col = 0;
    update_cursor();
}

void terminal_init(void) {
    esc_state = 0;
    reverse = 0;
    cur_fg = VGA_LIGHT_GREY;
    cur_bg = VGA_BLACK;
    saved_row = saved_col = 0;
    terminal_clear();
}

void terminal_putc(char c) {
    put_char(c);
    update_cursor();
}

void terminal_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) put_char(s[i]);
    update_cursor();                            // once per write, not per character
}

void terminal_puts(const char *s) {
    terminal_write(s, strlen(s));
}