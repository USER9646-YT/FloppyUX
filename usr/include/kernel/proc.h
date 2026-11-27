#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <stdint.h>
#include <stddef.h>
#include <sys/procinfo.h>
#include "idt.h"
#include "vfs.h"

enum proc_state {
    P_UNUSED = 0,
    P_EMBRYO,        // being created
    P_RUNNABLE,
    P_RUNNING,
    P_SLEEP_TTY,     // waiting for keyboard input
    P_SLEEP_WAIT,    // waiting for a child to exit
    P_SLEEP_PIPE,    // waiting for data or room in a pipe
    P_SLEEP_TIMER,   // sleeping until wake_tick
    P_ZOMBIE         // exited, waiting to be reaped
};

struct process {
    int              pid;
    int              ppid;
    enum proc_state  state;
    uint32_t         dir;                   // physical address of the page directory
    uint8_t         *kstack;                // kernel stack (heap allocation)
    uint32_t         ksp;                   // saved kernel stack pointer
    int              exit_code;
    uint32_t         uid;                   // credentials (0 = root)
    uint32_t         gid;
    char             name[16];              // program name, for ps / pkill
    int              killed;                // pending fatal signal (0 = none)
    uint32_t         wake_tick;             // for P_SLEEP_TIMER
    struct vfs_file *fds[VFS_MAX_FDS];      // open files; 0, 1, 2 = console
    char             cwd[VFS_PATH_MAX];     // current directory (absolute)
};

extern struct process *current;

int  proc_spawn_init(const char *path);
void proc_start(void) __attribute__((noreturn));

void schedule(void);
void yield(void);
void sched_sleep(enum proc_state why);
void sched_wake(enum proc_state why);

int  proc_fork(struct registers *r);
int  proc_exec(struct registers *r, const char *path, char *const *argv);
int  proc_waitpid(int pid, int *status, int options);     // options: 1 = WNOHANG
void proc_exit(int code) __attribute__((noreturn));

int  proc_signal(int pid, int sig);            // 0 or -errno
void proc_tick(uint32_t ticks);                // called from the timer interrupt
int  proc_sleep(uint32_t nticks);              // 0, or -EINTR if a signal arrived
int  proc_info(int index, struct procinfo *out);
int  proc_cwd_under(const char *path);         // 1 if any live process has its cwd in path

// Copies a NUL-terminated string from user space; returns its length or -errno
int  copy_str_from_user(char *dst, const char *src, size_t max);

#endif