// Early UNIX-style Init system
// Practically useless unlike you want it very minimal (this means, you need to make the kernel mount the VFSes or do it manually, unlike rc.sysinit mounting them. Also, diskd wont work unless you manually start it)

#include "unistd.h"
#include "ulib.h"

int main(void) {
    puts("Starting sysinit runlevel\n");
    puts("init: starting /bin/login\n");

    for (;;) {
        int pid = fork();

        if (pid < 0) {
            puts("init: fork failed\n");
            for (;;) ;                         // Never exit, since exiting causes a panic
        }

        if (pid == 0) {
            char *argv[] = { "login", 0 };
            execve("/bin/login", argv, 0);     // We still want a bit of security here, so you need to log in
            puts("init: cannot exec /bin/login\n");
            _exit(127);
        }

        // Reap anything that exit (including orphans) until the shell is gone
        int status, w;
        do {
            w = waitpid(-1, &status, 0);
        } while (w != pid && w > 0);

        puts("init: shell exited, restarting\n");
    }
}