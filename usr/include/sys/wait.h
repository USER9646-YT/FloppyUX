#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fallback options if not already defined in <unistd.h> */
#ifndef WNOHANG
#define WNOHANG    0x00000001
#endif

#ifndef WUNTRACED
#define WUNTRACED  0x00000002
#endif

#ifndef WCONTINUED
#define WCONTINUED 0x00000008
#endif

/* Standard POSIX Status Helpers */
#define _WSTATUS(x)     ((x) & 0x7f)
#define _WSTOPPED       0x7f
#define _WCONTINUED     0xffff

#ifndef WIFEXITED
#define WIFEXITED(s)    (_WSTATUS(s) == 0)
#endif

#ifndef WEXITSTATUS
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#endif

#ifndef WIFSIGNALED
#define WIFSIGNALED(s)  (_WSTATUS(s) != 0 && _WSTATUS(s) != _WSTOPPED)
#endif

#ifndef WTERMSIG
#define WTERMSIG(s)     (_WSTATUS(s))
#endif

#ifndef WCOREDUMP
#define WCOREDUMP(s)    ((s) & 0x80)
#endif

#ifndef WIFSTOPPED
#define WIFSTOPPED(s)   (_WSTATUS(s) == _WSTOPPED)
#endif

#ifndef WSTOPSIG
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#endif

#ifndef WIFCONTINUED
#define WIFCONTINUED(s) ((s) == _WCONTINUED)
#endif

/* Provide wait() as an inline wrapper around your unistd.h waitpid() */
// static inline pid_t wait(int *stat_loc) {
//     return waitpid(-1, stat_loc, 0);
// }

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
