#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "proc.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "elf.h"
#include "vfs.h"
#include "tty.h"
#include "pit.h"
#include "kprintf.h"
#include "terminal.h"

#define MAX_PROCS        32
#define KSTACK_SIZE      8192
#define USER_STACK_TOP   0x80000000u
#define USER_STACK_PAGES 4
#define MAX_ARGS         16
#define ARG_BYTES        2048

static struct process procs[MAX_PROCS];
struct process *current;
static int      next_pid = 1;
static uint32_t boot_ksp;

extern void switch_context(uint32_t *old_ksp, uint32_t new_ksp);
extern void trap_return(void);

// ---------- helpers ---------- //

static struct process *alloc_proc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == P_UNUSED) {
            memset(&procs[i], 0, sizeof(procs[i]));
            procs[i].pid   = next_pid++;
            procs[i].state = P_EMBRYO;
            return &procs[i];
        }
    }
    return NULL;
}

static void release_proc(struct process *p) {
    kfree(p->kstack);
    p->kstack = NULL;
    p->state  = P_UNUSED;
}

// The process name is the last component of the program path
static void set_name(struct process *p, const char *path) {
    const char *base = path;
    for (const char *c = path; *c; c++)
        if (*c == '/' && c[1]) base = c + 1;

    size_t i = 0;
    while (base[i] && i < sizeof(p->name) - 1) {
        p->name[i] = base[i];
        i++;
    }
    p->name[i] = '\0';
}

// Prepares the kernel stack so that the first switch to `p` "returns" into
// trap_return, which pops the trap frame and irets into user mode.
static void setup_kstack(struct process *p, const struct registers *tf) {
    uint32_t *top = (uint32_t *)(p->kstack + KSTACK_SIZE);
    struct registers *ctf = (struct registers *)top - 1;
    *ctf = *tf;

    uint32_t *sp = (uint32_t *)ctf;
    *--sp = (uint32_t)(uintptr_t)trap_return;     // popped by 'ret' in switch_context
    *--sp = 0;                                    // ebp
    *--sp = 0;                                    // ebx
    *--sp = 0;                                    // esi
    *--sp = 0;                                    // edi
    p->ksp = (uint32_t)(uintptr_t)sp;
}

static void set_tss_stack(struct process *p) {
    gdt_set_kernel_stack((uint32_t)(uintptr_t)(p->kstack + KSTACK_SIZE));
}

int copy_str_from_user(char *dst, const char *src, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (!paging_check_user((uint32_t)(uintptr_t)(src + i), 1, 0)) return -EFAULT;
        dst[i] = src[i];
        if (!dst[i]) return (int)i;
    }
    return -ENAMETOOLONG;
}

// Copies argv from user space into `args` (packed strings) and `offs`.
static int collect_args(char *const *uargv, char *args, uint32_t *offs, int *argc_out) {
    uint32_t used = 0;
    int argc = 0;

    if (uargv) {
        for (;;) {
            if (argc == MAX_ARGS) return -E2BIG;
            if (!paging_check_user((uint32_t)(uintptr_t)(uargv + argc), sizeof(char *), 0))
                return -EFAULT;

            const char *p = uargv[argc];
            if (!p) break;

            int n = copy_str_from_user(args + used, p, ARG_BYTES - used);
            if (n < 0) return n == -ENAMETOOLONG ? -E2BIG : n;

            offs[argc++] = used;
            used += (uint32_t)n + 1;
        }
    }
    *argc_out = argc;
    return 0;
}

// Maps a fresh user stack into `dir` and writes argc/argv/envp on it.
static int build_user_stack(uint32_t dir, int argc, const char *args,
                            const uint32_t *offs, uint32_t *esp_out) {
    uint32_t top_frame = 0;

    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t frame = pmm_alloc();
        if (!frame) return -ENOMEM;
        memset((void *)frame, 0, PAGE_SIZE);
        if (paging_map_in(dir, USER_STACK_TOP - (i + 1) * PAGE_SIZE, frame,
                          PTE_USER | PTE_WRITE) != 0) {
            pmm_free(frame);
            return -ENOMEM;
        }
        if (i == 0) top_frame = frame;
    }

    // Everything is written into the top page; sp is an offset inside it.
    uint8_t *page = (uint8_t *)top_frame;
    const uint32_t base = USER_STACK_TOP - PAGE_SIZE;     // virtual address of that page
    uint32_t argv_va[MAX_ARGS];
    uint32_t sp = PAGE_SIZE;

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(args + offs[i]) + 1;
        sp -= (uint32_t)len;
        memcpy(page + sp, args + offs[i], len);
        argv_va[i] = base + sp;
    }

    sp &= ~15u;
    sp -= (uint32_t)(argc + 3) * 4;              // argc, argv[argc], NULL, NULL
    sp &= ~15u;

    uint32_t *w = (uint32_t *)(page + sp);
    w[0] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) w[1 + i] = argv_va[i];
    w[1 + argc] = 0;                             // end of argv
    w[2 + argc] = 0;                             // end of envp (empty)

    *esp_out = base + sp;
    return 0;
}

// ---------- scheduler ---------- //

void schedule(void) {
    struct process *prev = current;
    struct process *next = NULL;

    for (;;) {
        int start = (int)(prev - procs);
        for (int i = 1; i <= MAX_PROCS; i++) {
            struct process *p = &procs[(start + i) % MAX_PROCS];
            if (p->state == P_RUNNABLE) { next = p; break; }
        }
        if (next) break;

        // Nothing to run: wait for an interrupt (keyboard, timer) to wake someone
        __asm__ volatile ("sti; hlt; cli");
    }

    if (next == prev) { prev->state = P_RUNNING; return; }

    current     = next;
    vfs_set_creds(next->uid, next->gid);
    next->state = P_RUNNING;
    set_tss_stack(next);
    paging_switch(next->dir);
    switch_context(&prev->ksp, next->ksp);
}

void yield(void) {
    current->state = P_RUNNABLE;
    schedule();
}

void sched_sleep(enum proc_state why) {
    current->state = why;
    schedule();
}

void sched_wake(enum proc_state why) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == why) procs[i].state = P_RUNNABLE;
}

// ---------- creating the first process ---------- //

int proc_spawn_init(const char *path) {
    struct process *p = alloc_proc();
    if (!p) return -EAGAIN;

    uint32_t entry, esp;
    char args[8] = "init";
    uint32_t offs[1] = { 0 };
    int r;

    p->kstack = kmalloc(KSTACK_SIZE);
    p->dir    = paging_create_space();
    if (!p->kstack || !p->dir) { r = -ENOMEM; goto fail; }

    r = elf_load(path, p->dir, &entry);
    if (r) goto fail;
    r = build_user_stack(p->dir, 1, args, offs, &esp);
    if (r) goto fail;

    struct registers tf;
    memset(&tf, 0, sizeof(tf));
    tf.gs = tf.fs = tf.es = tf.ds = 0x23;
    tf.eip      = entry;
    tf.cs       = 0x1B;
    tf.eflags   = 0x202;                     // IF set
    tf.useresp  = esp;
    tf.ss       = 0x23;

    setup_kstack(p, &tf);

    for (int i = 0; i < 3; i++)
        p->fds[i] = vfs_file_alloc(tty_console(), O_RDWR);
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    set_name(p, "init");

    p->state = P_RUNNABLE;
    return 0;

fail:
    if (p->dir) paging_destroy_space(p->dir);
    release_proc(p);
    return r;
}

void proc_start(void) {
    struct process *p = NULL;
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_RUNNABLE) { p = &procs[i]; break; }

    if (!p) {
        kprintf("proc: nothing to run\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    current  = p;
    vfs_set_creds(p->uid, p->gid);
    p->state = P_RUNNING;
    set_tss_stack(p);
    paging_switch(p->dir);
    switch_context(&boot_ksp, p->ksp);        // never comes back

    for (;;) __asm__ volatile ("hlt");
}

// ---------- fork / exec / exit / wait ---------- //

int proc_fork(struct registers *r) {
    struct process *c = alloc_proc();
    if (!c) return -EAGAIN;

    c->kstack = kmalloc(KSTACK_SIZE);
    c->dir    = paging_clone_space(current->dir);
    if (!c->kstack || !c->dir) {
        if (c->dir) paging_destroy_space(c->dir);
        release_proc(c);
        return -ENOMEM;
    }

    struct registers tf = *r;
    tf.eax = 0;                               // fork returns 0 in the child
    setup_kstack(c, &tf);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (current->fds[i]) {
            vfs_file_ref(current->fds[i]);
            c->fds[i] = current->fds[i];
        }
    }
    memcpy(c->cwd, current->cwd, sizeof(c->cwd));
    memcpy(c->name, current->name, sizeof(c->name));
    c->uid = current->uid;
    c->gid = current->gid;

    c->ppid  = current->pid;
    c->state = P_RUNNABLE;
    return c->pid;
}

int proc_exec(struct registers *r, const char *upath, char *const *uargv) {
    char *path = kmalloc(VFS_PATH_MAX);
    char *args = kmalloc(ARG_BYTES);
    uint32_t offs[MAX_ARGS];
    uint32_t dir = 0, entry = 0, esp = 0, old;
    int argc = 0, ret, n;

    if (!path || !args) { ret = -ENOMEM; goto out; }

    n = copy_str_from_user(path, upath, VFS_PATH_MAX);
    if (n < 0) { ret = n; goto out; }

    ret = collect_args(uargv, args, offs, &argc);
    if (ret) goto out;

    if (argc == 0) {                          // no argv given: use the path
        memcpy(args, path, (size_t)n + 1);
        offs[0] = 0;
        argc = 1;
    }

    ret = vfs_access_path(path, ACC_X);       // may this user run it?
    if (ret) goto out;

    dir = paging_create_space();
    if (!dir) { ret = -ENOMEM; goto out; }

    // Build the new image first, so a failure leaves the old program intact
    ret = elf_load(path, dir, &entry);
    if (ret) goto fail;
    ret = build_user_stack(dir, argc, args, offs, &esp);
    if (ret) goto fail;

    old = current->dir;
    current->dir = dir;
    paging_switch(dir);
    paging_destroy_space(old);

    set_name(current, path);

    r->eip     = entry;
    r->useresp = esp;
    r->eflags  = 0x202;
    r->eax = r->ebx = r->ecx = r->edx = 0;
    r->esi = r->edi = r->ebp = 0;

    kfree(path);
    kfree(args);
    return 0;

fail:
    paging_destroy_space(dir);
out:
    kfree(path);
    kfree(args);
    return ret;
}

static void panic_init_exit(int code) {
    __asm__ volatile ("cli");
    terminal_setcolor(VGA_WHITE, VGA_RED);
    kprintf("\nKernel panic - not syncing: attempted to kill init! (exit code %d)\n", code);
    kprintf("System halted.\n");
    for (;;) __asm__ volatile ("hlt");
}

void proc_exit(int code) {
    struct process *p = current;

    if (p->pid == 1) panic_init_exit(code);

    p->exit_code = code;

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        vfs_file_put(p->fds[i]);
        p->fds[i] = NULL;
    }

    // Leave the dying address space, then free it
    paging_switch(paging_kernel_dir());
    paging_destroy_space(p->dir);
    p->dir = 0;

    // Orphans become children of init
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != P_UNUSED && procs[i].state != P_EMBRYO &&
            procs[i].ppid == p->pid)
            procs[i].ppid = 1;

    p->state = P_ZOMBIE;
    sched_wake(P_SLEEP_WAIT);                 // the parent (or init) may be waiting
    schedule();

    for (;;) __asm__ volatile ("hlt");        // never reached
}

int proc_waitpid(int pid, int *status, int options) {
    for (;;) {
        int have_child = 0;

        for (int i = 0; i < MAX_PROCS; i++) {
            struct process *q = &procs[i];
            if (q->state == P_UNUSED || q->state == P_EMBRYO) continue;
            if (q->ppid != current->pid) continue;
            if (pid > 0 && q->pid != pid) continue;

            have_child = 1;
            if (q->state == P_ZOMBIE) {
                int cpid = q->pid;
                *status = (q->exit_code & 0xFF) << 8;
                release_proc(q);
                return cpid;
            }
        }

        if (!have_child) return -ECHILD;
        if (options & 1) return 0;            // WNOHANG: children exist, none has exited

        sched_sleep(P_SLEEP_WAIT);
        if (current->killed) return -EINTR;
    }
}

// ---------- signals, sleeping, process list ---------- //

int proc_signal(int pid, int sig) {
    if (sig < 0 || sig > 31 || pid <= 0) return -EINVAL;

    struct process *p = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].pid == pid && procs[i].state != P_UNUSED && procs[i].state != P_EMBRYO) {
            p = &procs[i];
            break;
        }
    }
    if (!p || p->state == P_ZOMBIE) return -ESRCH;

    if (current->uid != 0 && current->uid != p->uid) return -EPERM;
    if (sig == 0) return 0;                       // just checking that it exists
    if (pid == 1) return -EPERM;                  // init can't be killed
    if (sig == 17 || sig == 18) return 0;         // SIGCHLD, SIGCONT: ignored
    if (sig > 15) return -EINVAL;                 // stop / real-time signals: not implemented

    p->killed = sig;                              // every implemented signal terminates

    if (p->state == P_SLEEP_TTY || p->state == P_SLEEP_WAIT ||
        p->state == P_SLEEP_PIPE || p->state == P_SLEEP_TIMER)
        p->state = P_RUNNABLE;                    // wake it so it notices
    return 0;
}

void proc_tick(uint32_t ticks) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_SLEEP_TIMER &&
            (int32_t)(ticks - procs[i].wake_tick) >= 0)
            procs[i].state = P_RUNNABLE;
}

int proc_sleep(uint32_t nticks) {
    current->wake_tick = pit_ticks() + nticks;

    while (!current->killed && (int32_t)(pit_ticks() - current->wake_tick) < 0)
        sched_sleep(P_SLEEP_TIMER);

    return current->killed ? -EINTR : 0;
}

int proc_info(int index, struct procinfo *out) {
    if (index < 0 || index >= MAX_PROCS) return -EINVAL;

    struct process *p = &procs[index];
    if (p->state == P_UNUSED || p->state == P_EMBRYO) return 0;

    out->pid  = p->pid;
    out->ppid = p->ppid;
    out->uid  = (int)p->uid;
    switch (p->state) {
    case P_RUNNABLE:
    case P_RUNNING: out->state = 'R'; break;
    case P_ZOMBIE:  out->state = 'Z'; break;
    default:        out->state = 'S'; break;
    }
    memcpy(out->name, p->name, sizeof(out->name));
    return 1;
}

int proc_cwd_under(const char *path) {
    size_t n = strlen(path);

    for (int i = 0; i < MAX_PROCS; i++) {
        struct process *p = &procs[i];
        if (p->state == P_UNUSED || p->state == P_EMBRYO || p->state == P_ZOMBIE) continue;
        if (strncmp(p->cwd, path, n) == 0 && (p->cwd[n] == '\0' || p->cwd[n] == '/'))
            return 1;
    }
    return 0;
}