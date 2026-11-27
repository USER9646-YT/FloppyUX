#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/procinfo.h>
#include <sys/reboot.h>
#include <dirent.h>
#include "syscall.h"
#include "idt.h"
#include "paging.h"
#include "vfs.h"
#include "proc.h"
#include "tty.h"
#include "pipe.h"
#include "pit.h"
#include "power.h"
#include "terminal.h"
#include "ramfs.h"
#include "devfs.h"
#include "procfs.h"

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_WAITPID   7
#define SYS_UNLINK   10
#define SYS_EXECVE   11
#define SYS_CHDIR    12
#define SYS_CHMOD    15
#define SYS_CHOWN    16
#define SYS_LSEEK    19
#define SYS_GETPID   20
#define SYS_MOUNT    21
#define SYS_UMOUNT   22
#define SYS_SETUID   23
#define SYS_GETUID   24
#define SYS_SYNC     36
#define SYS_KILL     37
#define SYS_RENAME   38
#define SYS_MKDIR    39
#define SYS_RMDIR    40
#define SYS_DUP      41
#define SYS_PIPE     42
#define SYS_SETGID   46
#define SYS_GETGID   47
#define SYS_IOCTL    54
#define SYS_DUP2     63
#define SYS_GETPPID  64
#define SYS_REBOOT   88
#define SYS_READDIR  89
#define SYS_STAT    106
#define SYS_UNAME   122
#define SYS_NANOSLEEP 162
#define SYS_GETCWD  183
#define SYS_PROCINFO 400

static struct vfs_file *get_fd(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return NULL;
    return current->fds[fd];
}

static int install_fd(struct vfs_file *f) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

// Copies a path from user space and makes it absolute using the process's cwd
static int user_path(const char *upath, char *out) {
    char tmp[VFS_PATH_MAX];
    int n = copy_str_from_user(tmp, upath, sizeof(tmp));
    if (n < 0) return n;
    if (n == 0) return -ENOENT;

    if (tmp[0] == '/') {
        memcpy(out, tmp, (size_t)n + 1);
        return 0;
    }

    size_t cl = strlen(current->cwd);
    if (cl + 1 + (size_t)n >= VFS_PATH_MAX) return -ENAMETOOLONG;

    memcpy(out, current->cwd, cl);
    out[cl] = '/';
    memcpy(out + cl + 1, tmp, (size_t)n + 1);
    return 0;                       // vfs normalizes "//", "." and ".."
}

static int sys_write(int fd, const char *buf, uint32_t len) {
    struct vfs_file *f = get_fd(fd);
    if (!f) return -EBADF;
    if (len == 0) return 0;
    if (len > 65536) len = 65536;                         // short writes are legal
    if (!paging_check_user((uint32_t)(uintptr_t)buf, len, 0)) return -EFAULT;
    return vfs_file_write(f, buf, len);
}

static int sys_read(int fd, char *buf, uint32_t len) {
    struct vfs_file *f = get_fd(fd);
    if (!f) return -EBADF;
    if (len == 0) return 0;
    if (len > 65536) len = 65536;
    if (!paging_check_user((uint32_t)(uintptr_t)buf, len, 1)) return -EFAULT;
    return vfs_file_read(f, buf, len);
}

static int sys_open(const char *upath, int flags) {
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    if (r) return r;

    int err = 0;
    struct vfs_file *f = vfs_file_open(path, flags, &err);
    if (!f) return err;

    int fd = install_fd(f);
    if (fd < 0) vfs_file_put(f);
    return fd;
}

static int sys_close(int fd) {
    struct vfs_file *f = get_fd(fd);
    if (!f) return -EBADF;

    current->fds[fd] = NULL;
    vfs_file_put(f);
    return 0;
}

static int sys_dup(int fd) {
    struct vfs_file *f = get_fd(fd);
    if (!f) return -EBADF;

    vfs_file_ref(f);
    int n = install_fd(f);
    if (n < 0) vfs_file_put(f);
    return n;
}

static int sys_dup2(int oldfd, int newfd) {
    struct vfs_file *f = get_fd(oldfd);
    if (!f) return -EBADF;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (current->fds[newfd]) vfs_file_put(current->fds[newfd]);     // close what was there
    vfs_file_ref(f);
    current->fds[newfd] = f;
    return newfd;
}

static int sys_pipe(int *ufds) {
    if (!paging_check_user((uint32_t)(uintptr_t)ufds, 2 * sizeof(int), 1)) return -EFAULT;

    struct vfs_file *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r) return r;

    int fd0 = install_fd(rd);
    if (fd0 < 0) {
        vfs_file_put(rd);
        vfs_file_put(wr);
        return fd0;
    }

    int fd1 = install_fd(wr);
    if (fd1 < 0) {
        current->fds[fd0] = NULL;
        vfs_file_put(rd);
        vfs_file_put(wr);
        return fd1;
    }

    ufds[0] = fd0;
    ufds[1] = fd1;
    return 0;
}

static int sys_lseek(int fd, int off, int whence) {
    struct vfs_file *f = get_fd(fd);
    if (!f) return -EBADF;
    return vfs_file_lseek(f, off, whence);
}

static int sys_stat(const char *upath, struct stat *ust) {
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    if (r) return r;
    if (!paging_check_user((uint32_t)(uintptr_t)ust, sizeof(*ust), 1)) return -EFAULT;

    struct vfs_stat vs;
    r = vfs_stat(path, &vs);
    if (r) return r;

    ust->st_ino  = vs.ino;
    ust->st_mode = vs.mode;
    ust->st_uid  = vs.uid;
    ust->st_gid  = vs.gid;
    ust->st_size = vs.size;
    return 0;
}

static int sys_readdir(int fd, struct dirent *ud) {
    struct vfs_file *f = get_fd(fd);
    if (!f) return -EBADF;
    if (!paging_check_user((uint32_t)(uintptr_t)ud, sizeof(*ud), 1)) return -EFAULT;

    struct vfs_dirent de;
    int r = vfs_file_readdir(f, &de);
    if (r <= 0) return r;

    ud->d_ino  = de.ino;
    ud->d_type = de.type;
    memcpy(ud->d_name, de.name, strlen(de.name) + 1);
    return 1;
}

static int sys_chdir(const char *upath) {
    char path[VFS_PATH_MAX], norm[VFS_PATH_MAX];
    int r = user_path(upath, path);
    if (r) return r;

    r = vfs_normalize(path, norm);
    if (r) return r;

    struct vfs_stat vs;
    r = vfs_stat(norm, &vs);
    if (r) return r;
    if (vs.type != VFS_DIR) return -ENOTDIR;

    memcpy(current->cwd, norm, strlen(norm) + 1);
    return 0;
}

static int sys_getcwd(char *buf, uint32_t size) {
    uint32_t len = (uint32_t)strlen(current->cwd) + 1;
    if (size < len) return -ERANGE;
    if (!paging_check_user((uint32_t)(uintptr_t)buf, len, 1)) return -EFAULT;

    memcpy(buf, current->cwd, len);
    return (int)len;
}

static int sys_mkdir(const char *upath) {            // the mode argument is ignored for now
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    return r ? r : vfs_mkdir(path);
}

static int sys_rmdir(const char *upath) {
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    return r ? r : vfs_rmdir(path);
}

static int sys_unlink(const char *upath) {
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    return r ? r : vfs_unlink(path);
}

static int sys_rename(const char *uold, const char *unew) {
    char oldp[VFS_PATH_MAX], newp[VFS_PATH_MAX];
    int r = user_path(uold, oldp);
    if (r) return r;
    r = user_path(unew, newp);
    if (r) return r;
    return vfs_rename(oldp, newp);
}

// mount(source, target, fstype): only virtual filesystems, root only
static int sys_mount(const char *usrc, const char *utarget, const char *utype) {
    char path[VFS_PATH_MAX], norm[VFS_PATH_MAX], type[16];
    const char *fsname;
    (void)usrc;                                   // virtual filesystems have no source

    if (current->uid != 0) return -EPERM;

    int r = user_path(utarget, path);
    if (r) return r;
    r = vfs_normalize(path, norm);
    if (r) return r;

    r = copy_str_from_user(type, utype, sizeof(type));
    if (r < 0) return r == -ENAMETOOLONG ? -ENODEV : r;

    struct vfs_stat vs;
    r = vfs_stat(norm, &vs);
    if (r) return r;
    if (vs.type != VFS_DIR) return -ENOTDIR;

    struct vfs_node *root;
    if (strcmp(type, "proc") == 0)       { fsname = "proc";  root = procfs_create(); }
    else if (strcmp(type, "devfs") == 0) { fsname = "devfs"; root = devfs_create(); }
    else if (strcmp(type, "ramfs") == 0 || strcmp(type, "tmpfs") == 0)
                                         { fsname = "ramfs"; root = ramfs_create_root(); }
    else return -ENODEV;
    if (!root) return -ENOMEM;

    r = vfs_mount_type(norm, root, fsname);
    if (r) { vfs_node_put(root); return r; }
    return 0;
}

static int sys_umount(const char *utarget) {
    char path[VFS_PATH_MAX], norm[VFS_PATH_MAX];

    if (current->uid != 0) return -EPERM;

    int r = user_path(utarget, path);
    if (r) return r;
    r = vfs_normalize(path, norm);
    if (r) return r;
    return vfs_umount(norm);
}

static void fill_field(char *dst, const char *src) {
    memcpy(dst, src, strlen(src) + 1);              // every field has 65 bytes
}

static int sys_uname(struct utsname *u) {
    if (!paging_check_user((uint32_t)(uintptr_t)u, sizeof(*u), 1)) return -EFAULT;

    memset(u, 0, sizeof(*u));
    fill_field(u->sysname,  OS_SYSNAME);
    fill_field(u->nodename, OS_NODENAME);
    fill_field(u->release,  OS_RELEASE);
    fill_field(u->version,  OS_VERSION);
    fill_field(u->machine,  OS_MACHINE);
    return 0;
}

static int sys_waitpid(int pid, int *ustatus, int options) {
    if (ustatus && !paging_check_user((uint32_t)(uintptr_t)ustatus, sizeof(int), 1))
        return -EFAULT;

    int status = 0;
    int r = proc_waitpid(pid, &status, options);
    if (r > 0 && ustatus) *ustatus = status;
    return r;
}

static int sys_chmod(const char *upath, uint32_t mode) {
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    return r ? r : vfs_chmod(path, mode);
}

static int sys_chown(const char *upath, int uid, int gid) {
    char path[VFS_PATH_MAX];
    int r = user_path(upath, path);
    return r ? r : vfs_chown(path, uid, gid);
}

static int sys_setuid(uint32_t uid) {
    if (current->uid != 0 && uid != current->uid) return -EPERM;
    current->uid = uid;
    vfs_set_creds(current->uid, current->gid);
    return 0;
}

static int sys_setgid(uint32_t gid) {
    if (current->uid != 0 && gid != current->gid) return -EPERM;
    current->gid = gid;
    vfs_set_creds(current->uid, current->gid);
    return 0;
}

static int sys_ioctl(int fd, uint32_t req, uint32_t arg) {
    if (!get_fd(fd)) return -EBADF;

    switch (req) {
    case TIOCSECHO:
        tty_set_echo((int)arg);
        return 0;
    case TIOCSRAW:
        tty_set_raw((int)arg);
        return 0;
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize *)(uintptr_t)arg;
        if (!paging_check_user((uint32_t)(uintptr_t)ws, sizeof(*ws), 1)) return -EFAULT;
        ws->ws_row    = TERM_ROWS;
        ws->ws_col    = TERM_COLS;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static int sys_nanosleep(const struct timespec *ureq) {
    struct timespec req;

    if (!paging_check_user((uint32_t)(uintptr_t)ureq, sizeof(req), 0)) return -EFAULT;
    memcpy(&req, ureq, sizeof(req));
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) return -EINVAL;

    uint32_t hz  = pit_freq();
    uint32_t sec = (uint32_t)req.tv_sec;
    if (sec > 1000000u) sec = 1000000u;
    uint32_t ms    = (uint32_t)req.tv_nsec / 1000000u;
    uint32_t ticks = sec * hz + (ms * hz + 999u) / 1000u;
    if (ticks == 0 && (req.tv_sec || req.tv_nsec)) ticks = 1;

    return proc_sleep(ticks);
}

static int sys_reboot(uint32_t cmd) {
    if (current->uid != 0) return -EPERM;

    switch (cmd) {
    case RB_AUTOBOOT: power_reboot();
    case RB_HALT:     power_halt();
    case RB_POWEROFF: power_off();
    default:          return -EINVAL;
    }
}

static int sys_procinfo(int index, struct procinfo *u) {
    if (!paging_check_user((uint32_t)(uintptr_t)u, sizeof(*u), 1)) return -EFAULT;

    struct procinfo pi;
    int r = proc_info(index, &pi);
    if (r > 0) memcpy(u, &pi, sizeof(pi));
    return r;
}

static void syscall_handler(struct registers *r) {
    switch (r->eax) {
    case SYS_EXIT:
        proc_exit((int)r->ebx);                           // does not return
    case SYS_FORK:
        r->eax = (uint32_t)proc_fork(r);
        break;
    case SYS_READ:
        r->eax = (uint32_t)sys_read((int)r->ebx, (char *)(uintptr_t)r->ecx, r->edx);
        break;
    case SYS_WRITE:
        r->eax = (uint32_t)sys_write((int)r->ebx, (const char *)(uintptr_t)r->ecx, r->edx);
        break;
    case SYS_OPEN:
        r->eax = (uint32_t)sys_open((const char *)(uintptr_t)r->ebx, (int)r->ecx);
        break;
    case SYS_CLOSE:
        r->eax = (uint32_t)sys_close((int)r->ebx);
        break;
    case SYS_WAITPID:
        r->eax = (uint32_t)sys_waitpid((int)r->ebx, (int *)(uintptr_t)r->ecx, (int)r->edx);
        break;
    case SYS_UNLINK:
        r->eax = (uint32_t)sys_unlink((const char *)(uintptr_t)r->ebx);
        break;
    case SYS_EXECVE:
        r->eax = (uint32_t)proc_exec(r, (const char *)(uintptr_t)r->ebx,
                                     (char *const *)(uintptr_t)r->ecx);
        break;
    case SYS_CHDIR:
        r->eax = (uint32_t)sys_chdir((const char *)(uintptr_t)r->ebx);
        break;
    case SYS_CHMOD:
        r->eax = (uint32_t)sys_chmod((const char *)(uintptr_t)r->ebx, r->ecx);
        break;
    case SYS_CHOWN:
        r->eax = (uint32_t)sys_chown((const char *)(uintptr_t)r->ebx, (int)r->ecx, (int)r->edx);
        break;
    case SYS_LSEEK:
        r->eax = (uint32_t)sys_lseek((int)r->ebx, (int)r->ecx, (int)r->edx);
        break;
    case SYS_GETPID:
        r->eax = (uint32_t)current->pid;
        break;
    case SYS_GETPPID:
        r->eax = (uint32_t)current->ppid;
        break;
    case SYS_MOUNT:
        r->eax = (uint32_t)sys_mount((const char *)(uintptr_t)r->ebx,
                                     (const char *)(uintptr_t)r->ecx,
                                     (const char *)(uintptr_t)r->edx);
        break;
    case SYS_UMOUNT:
        r->eax = (uint32_t)sys_umount((const char *)(uintptr_t)r->ebx);
        break;
    case SYS_SETUID:
        r->eax = (uint32_t)sys_setuid(r->ebx);
        break;
    case SYS_GETUID:
        r->eax = current->uid;
        break;
    case SYS_SYNC:
        r->eax = 0;                                       // every write already goes straight to disk
        break;
    case SYS_KILL:
        r->eax = (uint32_t)proc_signal((int)r->ebx, (int)r->ecx);
        break;
    case SYS_RENAME:
        r->eax = (uint32_t)sys_rename((const char *)(uintptr_t)r->ebx,
                                      (const char *)(uintptr_t)r->ecx);
        break;
    case SYS_MKDIR:
        r->eax = (uint32_t)sys_mkdir((const char *)(uintptr_t)r->ebx);
        break;
    case SYS_RMDIR:
        r->eax = (uint32_t)sys_rmdir((const char *)(uintptr_t)r->ebx);
        break;
    case SYS_DUP:
        r->eax = (uint32_t)sys_dup((int)r->ebx);
        break;
    case SYS_PIPE:
        r->eax = (uint32_t)sys_pipe((int *)(uintptr_t)r->ebx);
        break;
    case SYS_SETGID:
        r->eax = (uint32_t)sys_setgid(r->ebx);
        break;
    case SYS_GETGID:
        r->eax = current->gid;
        break;
    case SYS_IOCTL:
        r->eax = (uint32_t)sys_ioctl((int)r->ebx, r->ecx, r->edx);
        break;
    case SYS_DUP2:
        r->eax = (uint32_t)sys_dup2((int)r->ebx, (int)r->ecx);
        break;
    case SYS_REBOOT:
        r->eax = (uint32_t)sys_reboot(r->ebx);
        break;
    case SYS_READDIR:
        r->eax = (uint32_t)sys_readdir((int)r->ebx, (struct dirent *)(uintptr_t)r->ecx);
        break;
    case SYS_STAT:
        r->eax = (uint32_t)sys_stat((const char *)(uintptr_t)r->ebx,
                                    (struct stat *)(uintptr_t)r->ecx);
        break;
    case SYS_UNAME:
        r->eax = (uint32_t)sys_uname((struct utsname *)(uintptr_t)r->ebx);
        break;
    case SYS_NANOSLEEP:
        r->eax = (uint32_t)sys_nanosleep((const struct timespec *)(uintptr_t)r->ebx);
        break;
    case SYS_GETCWD:
        r->eax = (uint32_t)sys_getcwd((char *)(uintptr_t)r->ebx, r->ecx);
        break;
    case SYS_PROCINFO:
        r->eax = (uint32_t)sys_procinfo((int)r->ebx, (struct procinfo *)(uintptr_t)r->ecx);
        break;
    default:
        r->eax = (uint32_t)-ENOSYS;
        break;
    }

    // a signal arrived while we were in the kernel: die before returning to user mode
    if (current->killed) proc_exit(128 + current->killed);
}

void syscall_init(void) {
    isr_register_handler(128, syscall_handler);
}