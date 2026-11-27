#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "procfs.h"
#include "vfs.h"
#include "proc.h"
#include "pit.h"
#include "pmm.h"
#include "kheap.h"
#include "floppy.h"

// Nothing is stored: every read renders its text from live kernel state.

enum { K_ROOT, K_PIDDIR, K_UPTIME, K_MEMINFO, K_VERSION, K_STATUS, K_CMDLINE, K_MOUNTS, K_FLOPPY };

struct pnode {
    int kind;
    int pid;                        // K_PIDDIR, K_STATUS, K_CMDLINE
};

struct entry { const char *name; int kind; };

static const struct entry top_files[] = {
    { "uptime",  K_UPTIME  },
    { "meminfo", K_MEMINFO },
    { "version", K_VERSION },
    { "mounts",  K_MOUNTS  },
    { "floppy",  K_FLOPPY  },
};
static const struct entry pid_files[] = {
    { "status",  K_STATUS  },
    { "cmdline", K_CMDLINE },
};
#define NTOP (int)(sizeof(top_files) / sizeof(top_files[0]))
#define NPID (int)(sizeof(pid_files) / sizeof(pid_files[0]))

#define GEN_MAX 1024

static struct vfs_ops procfs_ops;       // filled in at the bottom

// ---------- tiny text builder ---------- //

struct sb {
    char    *p;
    uint32_t len, cap;
};

static void sb_str(struct sb *s, const char *t) {
    while (*t && s->len < s->cap) s->p[s->len++] = *t++;
}

static void sb_u32(struct sb *s, uint32_t v) {
    char tmp[12];
    int  i = 0;

    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && s->len < s->cap) s->p[s->len++] = tmp[--i];
}

// ---------- process lookup (through proc_info) ---------- //

static int find_proc(int pid, struct procinfo *out) {
    for (int i = 0; proc_info(i, out) != -EINVAL; i++) {
        if (proc_info(i, out) == 1 && out->pid == pid) return 1;
    }
    return 0;
}

// pid of the n-th live process, or -1
static int pid_at(uint32_t n) {
    struct procinfo pi;

    for (int i = 0; proc_info(i, &pi) != -EINVAL; i++) {
        if (proc_info(i, &pi) != 1) continue;
        if (n-- == 0) return pi.pid;
    }
    return -1;
}

// ---------- content ---------- //

static int gen(const struct pnode *pn, char *buf, uint32_t cap) {
    struct sb s = { buf, 0, cap };
    struct procinfo pi;

    switch (pn->kind) {
    case K_UPTIME: {
        uint32_t hz = pit_freq() ? pit_freq() : 100;
        uint32_t t  = pit_ticks();
        uint32_t cs = (t % hz) * 100 / hz;

        sb_u32(&s, t / hz);
        sb_str(&s, ".");
        if (cs < 10) sb_str(&s, "0");
        sb_u32(&s, cs);
        sb_str(&s, "\n");
        break;
    }
    case K_MEMINFO: {
        uint32_t total = pmm_total_frames() * (PAGE_SIZE / 1024);
        uint32_t free_ = pmm_free_frames()  * (PAGE_SIZE / 1024);

        sb_str(&s, "MemTotal: "); sb_u32(&s, total);         sb_str(&s, " kB\n");
        sb_str(&s, "MemFree:  "); sb_u32(&s, free_);         sb_str(&s, " kB\n");
        sb_str(&s, "MemUsed:  "); sb_u32(&s, total - free_); sb_str(&s, " kB\n");
        break;
    }
    case K_VERSION:
        sb_str(&s, "FloppyUX version 0.1 (i386)\n");
        break;

    case K_MOUNTS: {
        const char *mp, *mt;
        for (int i = 0; vfs_mount_info(i, &mp, &mt); i++) {
            sb_str(&s, "none ");
            sb_str(&s, mp);
            sb_str(&s, " ");
            sb_str(&s, mt);
            sb_str(&s, "\n");
        }
        break;
    }
    
    case K_FLOPPY: {
        floppy_poll();                      // reading this file re-checks the drives
        int any = 0;
        for (int i = 0; i < FLOPPY_MAX_DRIVES; i++) {
            const struct floppy_drive *d = floppy_get(i);
            if (!d) continue;
            any = 1;
            sb_str(&s, "fd");  sb_u32(&s, (uint32_t)i);  sb_str(&s, ": ");
            sb_str(&s, d->model);
            if (d->media) { sb_str(&s, ", disk present, "); sb_u32(&s, d->sectors / 2); sb_str(&s, " KiB"); }
            else          { sb_str(&s, ", no disk"); }
            sb_str(&s, ", change #"); sb_u32(&s, d->generation);
            sb_str(&s, "\n");
        }
        if (!any) sb_str(&s, "no floppy drives\n");
        break;
    }

    case K_STATUS:
        if (!find_proc(pn->pid, &pi)) return -ESRCH;
        sb_str(&s, "Name:  ");  sb_str(&s, pi.name);        sb_str(&s, "\n");
        sb_str(&s, "State: ");
        sb_str(&s, pi.state == 'R' ? "R (running)" :
                   pi.state == 'Z' ? "Z (zombie)"  : "S (sleeping)");
        sb_str(&s, "\n");
        sb_str(&s, "Pid:   ");  sb_u32(&s, (uint32_t)pi.pid);  sb_str(&s, "\n");
        sb_str(&s, "PPid:  ");  sb_u32(&s, (uint32_t)pi.ppid); sb_str(&s, "\n");
        sb_str(&s, "Uid:   ");  sb_u32(&s, (uint32_t)pi.uid);  sb_str(&s, "\n");
        break;

    case K_CMDLINE:
        if (!find_proc(pn->pid, &pi)) return -ESRCH;
        sb_str(&s, pi.name);
        sb_str(&s, "\n");
        break;

    default:
        return -EINVAL;
    }
    return (int)s.len;
}

// ---------- nodes ---------- //

static struct vfs_node *new_node(int kind, int pid) {
    struct vfs_node *n  = kcalloc(1, sizeof(*n));
    struct pnode    *pn = kcalloc(1, sizeof(*pn));
    if (!n || !pn) { kfree(n); kfree(pn); return NULL; }

    int is_dir = (kind == K_ROOT || kind == K_PIDDIR);

    pn->kind = kind;
    pn->pid  = pid;

    n->ino  = ((uint32_t)pid << 8) | (uint32_t)kind;
    n->type = is_dir ? VFS_DIR : VFS_FILE;
    n->refs = 1;
    n->mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    n->ops  = &procfs_ops;
    n->priv = pn;
    return n;
}

static void procfs_release(struct vfs_node *n) {
    kfree(n->priv);
    kfree(n);
}

static int procfs_read(struct vfs_node *n, uint32_t off, uint32_t len, void *buf) {
    struct pnode *pn = n->priv;
    char tmp[GEN_MAX];

    if (n->type == VFS_DIR) return -EISDIR;

    int total = gen(pn, tmp, sizeof(tmp));
    if (total < 0) return total;

    if (off >= (uint32_t)total) return 0;
    if (len > (uint32_t)total - off) len = (uint32_t)total - off;
    memcpy(buf, tmp + off, len);
    return (int)len;
}

static int procfs_readdir(struct vfs_node *dir, uint32_t index, struct vfs_dirent *out) {
    struct pnode *pn = dir->priv;
    const char   *name;
    uint32_t      ino, type;

    if (pn->kind == K_ROOT) {
        if (index < (uint32_t)NTOP) {
            name = top_files[index].name;
            ino  = (uint32_t)top_files[index].kind;
            type = VFS_FILE;
        } else {
            int pid = pid_at(index - NTOP);
            if (pid < 0) return 0;

            // number as the name
            char num[12];
            int  i = 11;
            num[i] = '\0';
            for (uint32_t v = (uint32_t)pid; ; v /= 10) {
                num[--i] = (char)('0' + v % 10);
                if (v < 10) break;
            }
            size_t l = strlen(num + i);
            memcpy(out->name, num + i, l + 1);
            out->ino  = ((uint32_t)pid << 8) | K_PIDDIR;
            out->type = VFS_DIR;
            return 1;
        }
    } else if (pn->kind == K_PIDDIR) {
        if (index >= (uint32_t)NPID) return 0;
        name = pid_files[index].name;
        ino  = ((uint32_t)pn->pid << 8) | (uint32_t)pid_files[index].kind;
        type = VFS_FILE;
    } else {
        return -ENOTDIR;
    }

    size_t l = strlen(name);
    memcpy(out->name, name, l + 1);
    out->ino  = ino;
    out->type = type;
    return 1;
}

static struct vfs_node *procfs_finddir(struct vfs_node *dir, const char *name) {
    struct pnode *pn = dir->priv;

    if (pn->kind == K_ROOT) {
        for (int i = 0; i < NTOP; i++)
            if (strcmp(name, top_files[i].name) == 0)
                return new_node(top_files[i].kind, 0);

        int pid = 0;
        if (strcmp(name, "self") == 0 && current) {
            pid = current->pid;
        } else {
            size_t l = strlen(name);
            if (l == 0 || l > 9) return NULL;
            for (size_t i = 0; i < l; i++) {
                if (name[i] < '0' || name[i] > '9') return NULL;
                pid = pid * 10 + (name[i] - '0');
            }
        }

        struct procinfo pi;
        if (pid <= 0 || !find_proc(pid, &pi)) return NULL;
        return new_node(K_PIDDIR, pid);
    }

    if (pn->kind == K_PIDDIR) {
        struct procinfo pi;
        if (!find_proc(pn->pid, &pi)) return NULL;

        for (int i = 0; i < NPID; i++)
            if (strcmp(name, pid_files[i].name) == 0)
                return new_node(pid_files[i].kind, pn->pid);
    }
    return NULL;
}

// procfs is read-only
static int procfs_write(struct vfs_node *n, uint32_t off, uint32_t len, const void *buf) {
    (void)n; (void)off; (void)len; (void)buf;
    return -EROFS;
}
static int procfs_create_entry(struct vfs_node *dir, const char *name, uint32_t type,
                               struct vfs_node **out) {
    (void)dir; (void)name; (void)type; (void)out;
    return -EROFS;
}
static int procfs_setattr(struct vfs_node *n, int mode, int uid, int gid) {
    (void)n; (void)mode; (void)uid; (void)gid;
    return -EROFS;
}
static int procfs_unlink(struct vfs_node *dir, const char *name) {
    (void)dir; (void)name;
    return -EROFS;
}
static int procfs_rename(struct vfs_node *od, const char *on,
                         struct vfs_node *nd, const char *nn) {
    (void)od; (void)on; (void)nd; (void)nn;
    return -EROFS;
}
static int procfs_truncate(struct vfs_node *n, uint32_t size) {
    (void)n; (void)size;
    return -EROFS;
}

static struct vfs_ops procfs_ops = {
    .read     = procfs_read,
    .write    = procfs_write,
    .readdir  = procfs_readdir,
    .finddir  = procfs_finddir,
    .create   = procfs_create_entry,
    .release  = procfs_release,
    .setattr  = procfs_setattr,
    .unlink   = procfs_unlink,
    .rmdir    = procfs_unlink,
    .rename   = procfs_rename,
    .truncate = procfs_truncate,
};

struct vfs_node *procfs_create(void) {
    return new_node(K_ROOT, 0);
}