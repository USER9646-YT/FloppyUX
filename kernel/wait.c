#include <sys/wait.h>
#include <errno.h>

// #ifndef ENOSYS
// #define ENOSYS 38
// #endif

pid_t wait(int *stat_loc) {
    return waitpid(-1, stat_loc, 0);
}

// pid_t waitpid(pid_t pid, int *stat_loc, int options) {
//    (void)pid;
//    (void)options;
//
//    if (stat_loc) {
//        *stat_loc = 0;
//    }
//
//    // errno = ENOSYS;
//    return -1;
//}
