#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <stdint.h>

struct vfs_node;

// Cooked mode: blocks until a full line was typed. Raw mode: returns as soon as
// any key is available. Returns 0 on end-of-file (Ctrl-D at an empty line).
int tty_read(char *buf, uint32_t len);

void tty_set_echo(int on);      // typed characters are echoed (1) or not (0)
void tty_set_raw(int on);       // 1 = raw (no line editing, no echo), 0 = line-buffered

// The console as a VFS character device (stdin/stdout/stderr of every process)
struct vfs_node *tty_console(void);

#endif