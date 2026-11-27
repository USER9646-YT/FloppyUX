#include <stdint.h>
#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "proc.h"

// US-american keyboard layout (qwertz)
static const char normal[] =
    "\0\033" "1234567890-=\b\t" "qwertzuiop[]\n\0" "asdfghjkl;'`\0\\"
    "yxcvbnm,./\0*\0 ";
static const char shifted[] =
    "\0\033" "!@#$%^&*()_+\b\t" "QWERTZUIOP{}\n\0" "ASDFGHJKL:\"~\0|"
    "YXCVBNM<>?\0*\0 ";

// Half-assed german keyboard layout i wont use.
/*
static const char normal[] =
    "\0\033" "1234567890s=\b\t" "qwertzuiopu+\n\0" "asdfghjkloa^\0#"
    "yxcvbnm,.-\0*\0 ";
static const char shifted[] =
    "\0\033" "!\"$%&/()=?+\b\t" "QWERTZUIOPU*\n\0" "ASDFGHJKLOA^\0'"
    "YXCVBNM;:_\0*\0 ";
*/

#define BUF_SIZE 256
static volatile char     buf[BUF_SIZE];
static volatile uint32_t head, tail;

static int lshift, rshift, lctrl, rctrl, caps;
static int ext;                         // previous byte was 0xE0
static int pause_skip;                  // bytes left of the Pause key sequence

static void push(char c) {
    uint32_t next = (head + 1) % BUF_SIZE;
    if (next != tail) {                 // drop the key if the buffer is full
        buf[head] = c;
        head = next;
    }
}

// All or nothing, so an escape sequence is never cut in half
static void push_str(const char *s) {
    uint32_t room = (tail + BUF_SIZE - head - 1) % BUF_SIZE;
    uint32_t n = 0;
    while (s[n]) n++;
    if (n > room) return;
    while (*s) push(*s++);
}

static void handle_extended(uint8_t code, int release) {
    switch (code) {
    case 0x1D: rctrl = !release; return;                    // right ctrl
    case 0x1C: if (!release) push('\n'); return;            // keypad enter
    case 0x35: if (!release) push('/');  return;            // keypad slash
    default: break;
    }
    if (release) return;

    switch (code) {
    case 0x48: push_str("\033[A");  break;                  // up
    case 0x50: push_str("\033[B");  break;                  // down
    case 0x4D: push_str("\033[C");  break;                  // right
    case 0x4B: push_str("\033[D");  break;                  // left
    case 0x47: push_str("\033[H");  break;                  // home
    case 0x4F: push_str("\033[F");  break;                  // end
    case 0x49: push_str("\033[5~"); break;                  // page up
    case 0x51: push_str("\033[6~"); break;                  // page down
    case 0x52: push_str("\033[2~"); break;                  // insert
    case 0x53: push_str("\033[3~"); break;                  // delete
    default: break;
    }
}

static void kb_handler(struct registers *r) {
    (void)r;
    uint8_t sc = inb(0x60);

    if (pause_skip) { pause_skip--; return; }
    if (sc == 0xE1) { pause_skip = 5; return; }             // Pause key: ignore it
    if (sc == 0xE0) { ext = 1; return; }

    int release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (ext) {
        ext = 0;
        handle_extended(code, release);
        sched_wake(P_SLEEP_TTY);
        return;
    }

    if (code == 0x2A) { lshift = !release; return; }
    if (code == 0x36) { rshift = !release; return; }
    if (code == 0x1D) { lctrl  = !release; return; }
    if (code == 0x3A && !release) { caps = !caps; return; }
    if (release || code >= sizeof(normal) - 1) return;

    int shift = lshift || rshift;
    int ctrl  = lctrl || rctrl;
    char c = shift ? shifted[code] : normal[code];

    if (caps) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    if (ctrl) {
        if (c >= 'a' && c <= 'z')      c = (char)(c & 0x1F);
        else if (c >= 'A' && c <= 'Z') c = (char)(c & 0x1F);
        else if (c == '[')             c = 27;
    }
    if (!c) return;

    push(c);
    sched_wake(P_SLEEP_TTY);                    // wake whoever waits for input
}

void keyboard_init(void) {
    isr_register_handler(33, kb_handler);
    pic_unmask(1);
}

int keyboard_poll(void) {
    if (head == tail) return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return (unsigned char)c;
}

char keyboard_getchar(void) {
    while (head == tail)
        __asm__ volatile ("hlt");
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return c;
}