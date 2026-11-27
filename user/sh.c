#include <unistd.h>
#include <fcntl.h>
#include "ulib.h"

#define MAX_ARGS 15
#define MAX_CMDS 8

struct cmd {
    char *argv[MAX_ARGS + 1];
    int   argc;
    char *in;               // < file
    char *out;              // > file or >> file
    int   append;
};

static struct cmd cmds[MAX_CMDS];
static char wordbuf[256];   // all words of one command line, with quotes removed
static int  wordpos;
static char scriptbuf[4096];
static int  interactive;    // reading commands from the terminal (not a script or -c)

// What ended the pipeline that parse_line() just parsed
enum { SEP_END, SEP_AND, SEP_BG };
static int sep;

static void cmd_init(struct cmd *c) {
    c->argc    = 0;
    c->argv[0] = 0;
    c->in      = 0;
    c->out     = 0;
    c->append  = 0;
}

// Copies the word at *pp into wordbuf (removing quotes). Returns it, or 0 if there is none.
static char *parse_word(char **pp) {
    char *s = *pp;
    char *begin = s;
    char *start = wordbuf + wordpos;
    char quote = 0;
    int n = 0;

    while (*s) {
        if (quote) {
            if (*s == quote) { quote = 0; s++; continue; }
        } else {
            if (*s == '"' || *s == '\'') { quote = *s++; continue; }
            if (*s == ' ' || *s == '|' || *s == '<' || *s == '>' || *s == '&') break;
        }
        if (wordpos + n >= (int)sizeof(wordbuf) - 1) break;
        start[n++] = *s++;
    }
    start[n] = '\0';
    wordpos += n + 1;
    *pp = s;
    return s != begin ? start : 0;
}

// Parses one pipeline starting at *pp, up to the end of the line or a '&&' / '&'
// (which is recorded in `sep`); *pp is left just after it.
// Returns the number of commands in the pipeline, 0 for an empty line, -1 on a syntax error
static int parse_line(char **pp) {
    int nc = 0;
    char *p = *pp;

    cmd_init(&cmds[0]);
    wordpos = 0;
    sep = SEP_END;

    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;

        if (*p == '&') {
            if (p[1] == '&') { p += 2; sep = SEP_AND; }
            else             { p++;    sep = SEP_BG;  }
            break;
        }

        if (*p == '|') {
            p++;
            if (cmds[nc].argc == 0 || nc + 1 >= MAX_CMDS) return -1;
            nc++;
            cmd_init(&cmds[nc]);
            continue;
        }

        if (*p == '<' || *p == '>') {
            char op = *p++;
            int append = 0;
            if (op == '>' && *p == '>') { append = 1; p++; }
            while (*p == ' ') p++;

            char *w = parse_word(&p);
            if (!w) return -1;
            if (op == '<') {
                cmds[nc].in = w;
            } else {
                cmds[nc].out = w;
                cmds[nc].append = append;
            }
            continue;
        }

        char *w = parse_word(&p);
        if (!w) return -1;
        if (cmds[nc].argc >= MAX_ARGS) return -1;
        cmds[nc].argv[cmds[nc].argc++] = w;
        cmds[nc].argv[cmds[nc].argc] = 0;
    }

    *pp = p;
    if (cmds[nc].argc == 0)                // empty line, a trailing '|', or "&& cmd" / "& cmd"
        return (nc == 0 && sep == SEP_END) ? 0 : -1;
    return nc + 1;
}

static int has_slash(const char *s) {
    for (; *s; s++)
        if (*s == '/') return 1;
    return 0;
}

static void join_path(char *dst, unsigned int max, const char *dir, const char *name) {
    unsigned int i = 0;
    while (*dir && i < max - 1) dst[i++] = *dir++;
    while (*name && i < max - 1) dst[i++] = *name++;
    dst[i] = '\0';
}

// Runs a program: the path as typed, or /bin/<name>, then /sbin/<name> and lastly /usr/bin/.
// Only returns if none of them could be started.
static void exec_command(char **argv) {
    char path[64];

    if (has_slash(argv[0])) {
        join_path(path, sizeof(path), "", argv[0]);
        execve(path, argv, 0);
        return;
    }
    join_path(path, sizeof(path), "/bin/", argv[0]);
    execve(path, argv, 0);
    join_path(path, sizeof(path), "/sbin/", argv[0]);
    execve(path, argv, 0);
}

// Runs a builtin in the shell itself. Returns its exit status, or -1 if it is not a builtin.
static int builtin(struct cmd *c) {
    char **argv = c->argv;

    if (streq(argv[0], "exit")) {
        int code = 0;
        if (c->argc > 1)
            for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) code = code * 10 + (*s - '0');
        _exit(code);
    }
    if (streq(argv[0], "pid")) {
        print_int(getpid());
        puts("\n");
        return 0;
    }
    if (streq(argv[0], "help")) {
        puts("help pid cd pwd exit ls cat echo cp mv rm mkdir rmdir touch\n"
             "grep head tail wc tee vi clear sleep ps kill pkill chmod chown\n"
             "uname id whoami true false halt poweroff reboot telinit runlevel\n"
             "mount umount\n");
        return 0;
    }
    if (streq(argv[0], "cd")) {
        const char *target = c->argc > 1 ? argv[1] : "/";
        int r = chdir(target);
        if (r < 0) eprint("cd", target, r);
        return r < 0 ? 1 : 0;
    }
    if (streq(argv[0], "pwd")) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) > 0) { puts(cwd); puts("\n"); }
        return 0;
    }
    return -1;
}

static int run_pipeline(int n, int bg) {
    int pids[MAX_CMDS];
    int started = 0;
    int prev = -1;                       // read end of the previous pipe

    for (int i = 0; i < n; i++) {
        int pfd[2] = { -1, -1 };

        if (i < n - 1 && pipe(pfd) < 0) {
            puts("sh: cannot create pipe\n");
            break;
        }

        int pid = fork();
        if (pid < 0) {
            puts("sh: fork failed\n");
            if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
            break;
        }

        if (pid == 0) {
            if (prev >= 0)   { dup2(prev, 0);   close(prev); }
            if (pfd[1] >= 0) { dup2(pfd[1], 1); close(pfd[1]); }
            if (pfd[0] >= 0) close(pfd[0]);

            if (bg && i == 0 && !cmds[i].in) {      // a background job must not steal the terminal's input
                int nfd = open("/dev/null", O_RDONLY);
                if (nfd >= 0) { dup2(nfd, 0); close(nfd); }
            }
            if (cmds[i].in) {
                int fd = open(cmds[i].in, O_RDONLY);
                if (fd < 0) { eprint("ush", cmds[i].in, fd); _exit(1); }
                dup2(fd, 0);
                close(fd);
            }
            if (cmds[i].out) {
                int fd = open(cmds[i].out,
                              O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC));
                if (fd < 0) { eprint("ush", cmds[i].out, fd); _exit(1); }
                dup2(fd, 1);
                close(fd);
            }

            exec_command(cmds[i].argv);
            puts("sh: command not found: ");
            puts(cmds[i].argv[0]);
            puts("\n");
            _exit(127);
        }

        pids[started++] = pid;
        if (prev >= 0)   close(prev);    // the parent keeps no copies: otherwise nobody sees EOF
        if (pfd[1] >= 0) close(pfd[1]);
        prev = pfd[0];
    }
    if (prev >= 0) close(prev);

    if (bg) {                            // don't wait: the job keeps running
        if (interactive && started > 0) {
            puts("[");
            print_int(pids[started - 1]);
            puts("]\n");
        }
        return 0;
    }

    int status = 0;
    for (int i = 0; i < started; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (i == started - 1) status = st;
    }
    return status;
}

// Runs one command line; returns the exit status of the last command that ran.
// Pipelines can be chained with '&&' (run the next one only if this one succeeded)
// and a pipeline ending in '&' runs in the background.
static int execute_line(char *line) {
    char *p = line;
    int status = 0;
    int need_cmd = 0;                   // an '&&' was just seen: a command must follow

    for (;;) {
        while (*p == ' ') p++;
        if (!*p) {
            if (need_cmd) { puts("sh: syntax error\n"); return 2; }
            return status;
        }

        int nc = parse_line(&p);
        if (nc < 0) { puts("sh: syntax error\n"); return 2; }
        if (nc == 0) return status;

        int chain = (sep == SEP_AND);

        if (sep == SEP_BG) {
            run_pipeline(nc, 1);
            status = 0;
        } else {
            int r = (nc == 1 && !cmds[0].in && !cmds[0].out) ? builtin(&cmds[0]) : -1;
            status = (r >= 0) ? r : WEXITSTATUS(run_pipeline(nc, 0));
        }

        need_cmd = chain;
        if (chain && status != 0) {
            // Failed: skip the rest of the '&&' chain without running it
            while (sep == SEP_AND) {
                while (*p == ' ') p++;
                if (!*p) { puts("sh: syntax error\n"); return 2; }
                if (parse_line(&p) < 0) { puts("sh: syntax error\n"); return 2; }
            }
            need_cmd = 0;
        }
    }
}

static int run_script(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        eprint("sh", path, fd);
        return 127;
    }

    int total = 0, n;
    while (total < (int)sizeof(scriptbuf) - 1 &&
           (n = read(fd, scriptbuf + total, sizeof(scriptbuf) - 1 - (unsigned int)total)) > 0)
        total += n;
    close(fd);
    scriptbuf[total] = '\0';

    int status = 0;
    char *p = scriptbuf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        int last = (*eol == '\0');
        *eol = '\0';

        char line[128];
        int len = 0;
        for (const char *s = p; *s && len < (int)sizeof(line) - 1; s++)
            line[len++] = (*s == '\t' || *s == '\r') ? ' ' : *s;
        line[len] = '\0';

        char *s = line;
        while (*s == ' ') s++;
        if (*s && *s != '#') status = execute_line(s);

        if (last) break;
        p = eol + 1;
    }
    return status;
}

int main(int argc, char **argv) {
    char line[128];
    int reaped;

    if (argc >= 3 && streq(argv[1], "-c")) {
        int len = 0;
        for (const char *s = argv[2]; *s && len < (int)sizeof(line) - 1; s++) line[len++] = *s;
        line[len] = '\0';
        return execute_line(line);
    }
    if (argc >= 2)
        return run_script(argv[1]);
    
    interactive = 1;
    puts("\nFloppyUX Shell (sh) V2\n");
    puts("Welcome! Type 'help' for the list of built-in commands.\n");

    for (;;) {
#ifdef WNOHANG
        while (waitpid(-1, &reaped, WNOHANG) > 0) ;     // collect finished background jobs
#endif
        ioctl(0, TIOCSRAW, 0);                  // recover if a program left the tty in raw mode
        ioctl(0, TIOCSECHO, 1);
        puts(getuid() == 0 ? "# " : "$ ");

        int n = read(0, line, sizeof(line) - 1);
        if (n == 0) { puts("exit\n"); _exit(0); }       // Ctrl-D
        if (n < 0) continue;
        if (line[n - 1] == '\n') n--;
        line[n] = '\0';

        execute_line(line);
    }
}