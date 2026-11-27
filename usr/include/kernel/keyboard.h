#ifndef KERNEL_KEYBOARD_H
#define KERNEL_KEYBOARD_H

void keyboard_init(void);
char keyboard_getchar(void);   // blocks (hlt) until a key is available, kernel use only
int  keyboard_poll(void);      // next byte, or -1 if none is waiting

#endif