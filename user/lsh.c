/***************************************************************************//**
  @file         sh.c
  @brief        LSH with pipes (|) and redirection (<, >, >>) for FloppyUX
*******************************************************************************/

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif
#ifndef EOF
#define EOF (-1)
#endif

/* Fallback open flags in case fcntl.h defines only a subset */
#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_CREAT
#define O_CREAT  0x0040
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif
#ifndef O_APPEND
#define O_APPEND 0x0400
#endif

/* =========================================================================
 * Self-contained mini-libc helpers
 * ========================================================================= */

static size_t sh_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void sh_print(int fd, const char *s) {
    if (s) {
        write(fd, s, sh_strlen(s));
    }
}

static void sh_perror(const char *prefix) {
    if (prefix && *prefix) {
        sh_print(2, prefix);
        sh_print(2, ": command failed\n");
    } else {
        sh_print(2, "error\n");
    }
}

static int sh_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* Simple 64 KB static bump heap reset on every command prompt */
static unsigned char sh_heap[65536];
static size_t sh_heap_pos = 0;

static void *sh_malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 7) & ~((size_t)7);
    if (sh_heap_pos + size > sizeof(sh_heap)) return NULL;
    void *ptr = &sh_heap[sh_heap_pos];
    sh_heap_pos += size;
    return ptr;
}

static void *sh_realloc(void *ptr, size_t size) {
    if (!ptr) return sh_malloc(size);
    if (size == 0) return NULL;
    unsigned char *new_ptr = (unsigned char *)sh_malloc(size);
    if (new_ptr) {
        unsigned char *old_ptr = (unsigned char *)ptr;
        for (size_t i = 0; i < size; i++) {
            new_ptr[i] = old_ptr[i];
        }
    }
    return new_ptr;
}

static void sh_free(void *ptr) {
    (void)ptr;
}

static int sh_getchar(void) {
    unsigned char ch;
    ssize_t n = read(0, &ch, 1);
    if (n <= 0) return EOF;
    return (int)ch;
}

/* =========================================================================
 * execvp implementation using your OS's execve()
 * ========================================================================= */

static int _has_slash(const char *s) {
    while (*s) {
        if (*s++ == '/') return 1;
    }
    return 0;
}

static void _build_path(char *dst, size_t dst_size, const char *dir, const char *file) {
    size_t i = 0;
    while (*dir && i + 1 < dst_size) dst[i++] = *dir++;
    if (i + 1 < dst_size) dst[i++] = '/';
    while (*file && i + 1 < dst_size) dst[i++] = *file++;
    dst[i] = '\0';
}

static int sh_execvp(const char *file, char *const argv[]) {
    char *const empty_env[] = { NULL };

    if (!file || *file == '\0') {
        return -1;
    }

    if (_has_slash(file)) {
        return execve(file, argv, empty_env);
    }

    const char *search_paths[] = { "/bin", "/usr/bin", "." };
    char full_path[256];

    for (size_t i = 0; i < sizeof(search_paths) / sizeof(search_paths[0]); i++) {
        _build_path(full_path, sizeof(full_path), search_paths[i], file);
        execve(full_path, argv, empty_env);
    }

    return -1;
}

/* =========================================================================
 * LSH Builtins
 * ========================================================================= */

int lsh_cd(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);

char *builtin_str[] = {
    "cd",
    "help",
    "exit"
};

int (*builtin_func[]) (char **) = {
    &lsh_cd,
    &lsh_help,
    &lsh_exit
};

int lsh_num_builtins(void) {
    return sizeof(builtin_str) / sizeof(char *);
}

int lsh_cd(char **args) {
    if (args[1] == NULL) {
        sh_print(2, "lsh: expected argument to \"cd\"\n");
    } else {
        if (chdir(args[1]) != 0) {
            sh_perror("lsh");
        }
    }
    return 1;
}

int lsh_help(char **args) {
    (void)args;
    sh_print(1, "FloppyUX Shell (LSH)\n");
    sh_print(1, "Supports pipes (|) and redirection (<, >, >>).\n");
    sh_print(1, "Built-in commands:\n");

    for (int i = 0; i < lsh_num_builtins(); i++) {
        sh_print(1, "  ");
        sh_print(1, builtin_str[i]);
        sh_print(1, "\n");
    }
    return 1;
}

int lsh_exit(char **args) {
    (void)args;
    return 0;
}

static int run_builtin(char **args) {
    if (!args || !args[0]) return -1;
    for (int i = 0; i < lsh_num_builtins(); i++) {
        if (sh_strcmp(args[0], builtin_str[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }
    return -1;
}

/* =========================================================================
 * Tokenizer (splits whitespace AND operators |, <, >, >>)
 * ========================================================================= */

#define LSH_TOK_BUFSIZE 64

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\a';
}

static int is_op(char c) {
    return c == '|' || c == '<' || c == '>';
}

char **lsh_split_line(char *line) {
    int bufsize = LSH_TOK_BUFSIZE;
    int pos = 0;
    char **tokens = sh_malloc(bufsize * sizeof(char *));
    if (!tokens) {
        sh_print(2, "lsh: allocation error\n");
        _exit(EXIT_FAILURE);
    }

    const char *p = line;
    while (*p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;

        if (pos + 2 >= bufsize) {
            bufsize += LSH_TOK_BUFSIZE;
            tokens = sh_realloc(tokens, bufsize * sizeof(char *));
            if (!tokens) {
                sh_print(2, "lsh: allocation error\n");
                _exit(EXIT_FAILURE);
            }
        }

        if (is_op(*p)) {
            if (*p == '>' && *(p + 1) == '>') {
                char *tok = sh_malloc(3);
                tok[0] = '>'; tok[1] = '>'; tok[2] = '\0';
                tokens[pos++] = tok;
                p += 2;
            } else {
                char *tok = sh_malloc(2);
                tok[0] = *p; tok[1] = '\0';
                tokens[pos++] = tok;
                p++;
            }
        } else {
            const char *start = p;
            while (*p && !is_space(*p) && !is_op(*p)) p++;
            size_t len = (size_t)(p - start);
            char *tok = sh_malloc(len + 1);
            for (size_t i = 0; i < len; i++) tok[i] = start[i];
            tok[len] = '\0';
            tokens[pos++] = tok;
        }
    }

    tokens[pos] = NULL;
    return tokens;
}

/* =========================================================================
 * Redirection & Pipeline Execution
 * ========================================================================= */

struct cmd_stage {
    char **argv;
    char *in_file;
    char *out_file;
    int append;
};

static int parse_stages(char **tokens, struct cmd_stage *stages, int max_stages) {
    int n_stages = 0;
    int i = 0;

    while (tokens[i] != NULL) {
        if (n_stages >= max_stages) {
            sh_print(2, "lsh: too many pipes\n");
            return -1;
        }

        struct cmd_stage *st = &stages[n_stages];
        st->argv = sh_malloc(LSH_TOK_BUFSIZE * sizeof(char *));
        st->in_file = NULL;
        st->out_file = NULL;
        st->append = 0;

        int argc = 0;
        while (tokens[i] != NULL && sh_strcmp(tokens[i], "|") != 0) {
            if (sh_strcmp(tokens[i], "<") == 0) {
                if (!tokens[i + 1] || is_op(tokens[i + 1][0])) {
                    sh_print(2, "lsh: syntax error near '<'\n");
                    return -1;
                }
                st->in_file = tokens[i + 1];
                i += 2;
            } else if (sh_strcmp(tokens[i], ">") == 0 || sh_strcmp(tokens[i], ">>") == 0) {
                int is_app = (tokens[i][1] == '>');
                if (!tokens[i + 1] || is_op(tokens[i + 1][0])) {
                    sh_print(2, "lsh: syntax error near '>'\n");
                    return -1;
                }
                st->out_file = tokens[i + 1];
                st->append = is_app;
                i += 2;
            } else {
                st->argv[argc++] = tokens[i++];
            }
        }
        st->argv[argc] = NULL;

        if (argc == 0) {
            sh_print(2, "lsh: empty command in pipeline\n");
            return -1;
        }

        n_stages++;

        if (tokens[i] != NULL && sh_strcmp(tokens[i], "|") == 0) {
            i++;
            if (tokens[i] == NULL) {
                sh_print(2, "lsh: unexpected end of line after '|'\n");
                return -1;
            }
        }
    }

    return n_stages;
}

static int apply_redirections(const struct cmd_stage *st) {
    if (st->in_file) {
        int fd = open(st->in_file, O_RDONLY);
        if (fd < 0) {
            sh_perror(st->in_file);
            return -1;
        }
        if (fd != 0) {
            dup2(fd, 0);
            close(fd);
        }
    }

    if (st->out_file) {
        int flags = O_WRONLY | O_CREAT | (st->append ? O_APPEND : O_TRUNC);
        int fd = open(st->out_file, flags);
        if (fd < 0) {
            sh_perror(st->out_file);
            return -1;
        }
        if (fd != 1) {
            dup2(fd, 1);
            close(fd);
        }
    }

    return 0;
}

#define MAX_PIPE_STAGES 16
#define PIPE_FDS        2
#define PIPE_READ       0
#define PIPE_WRITE      1

int lsh_execute(char **tokens) {
    if (!tokens || !*tokens) {
        return 1;
    }

    struct cmd_stage stages[MAX_PIPE_STAGES];
    int n_stages = parse_stages(tokens, stages, MAX_PIPE_STAGES);
    if (n_stages <= 0) {
        return 1;
    }

    /* Single built-in command without pipes runs directly in parent shell */
    if (n_stages == 1 && !stages->in_file && !stages->out_file) {
        int b = run_builtin(stages->argv);
        if (b >= 0) return b;
    }

    pid_t pids[MAX_PIPE_STAGES];
    for (int i = 0; i < MAX_PIPE_STAGES; i++) {
        pids[i] = -1;
    }
    int prev_read_fd = -1;

    for (int s = 0; s < n_stages; s++) {
        int pipefd[PIPE_FDS] = { -1, -1 };
        if (s < n_stages - 1) {
            if (pipe(pipefd) < 0) {
                sh_perror("pipe");
                if (prev_read_fd >= 0) close(prev_read_fd);
                break;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            sh_perror("fork");
            if (prev_read_fd >= 0) close(prev_read_fd);
            if (pipefd[PIPE_READ] >= 0) close(pipefd[PIPE_READ]);
            if (pipefd[PIPE_WRITE] >= 0) close(pipefd[PIPE_WRITE]);
            break;
        }

        if (pid == 0) {
            /* Connect stdin from previous pipe stage */
            if (prev_read_fd >= 0) {
                dup2(prev_read_fd, 0);
                close(prev_read_fd);
            }
            /* Connect stdout to next pipe stage */
            if (pipefd[PIPE_WRITE] >= 0) {
                dup2(pipefd[PIPE_WRITE], 1);
                close(pipefd[PIPE_WRITE]);
            }
            if (pipefd[PIPE_READ] >= 0) {
                close(pipefd[PIPE_READ]);
            }

            /* File redirections (<, >) override pipe endpoints just like POSIX sh */
            if (apply_redirections(&stages[s]) < 0) {
                _exit(EXIT_FAILURE);
            }

            int b = run_builtin(stages[s].argv);
            if (b >= 0) {
                _exit(EXIT_SUCCESS);
            }

            if (sh_execvp(*stages[s].argv, stages[s].argv) == -1) {
                sh_perror(*stages[s].argv);
            }
            _exit(EXIT_FAILURE);
        }

        pids[s] = pid;

        if (prev_read_fd >= 0) {
            close(prev_read_fd);
        }
        if (pipefd[PIPE_WRITE] >= 0) {
            close(pipefd[PIPE_WRITE]);
        }
        prev_read_fd = pipefd[PIPE_READ];
    }

    for (int s = 0; s < n_stages; s++) {
        if (pids[s] > 0) {
            int status = 0;
            do {
                waitpid(pids[s], &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        }
    }

    return 1;
}

/* =========================================================================
 * Line Reader & Main Loop
 * ========================================================================= */

#define LSH_RL_BUFSIZE 1024
char *lsh_read_line(void) {
    int bufsize = LSH_RL_BUFSIZE;
    int position = 0;
    char *buffer = sh_malloc(sizeof(char) * bufsize);
    int c;

    if (!buffer) {
        sh_print(2, "lsh: allocation error\n");
        _exit(EXIT_FAILURE);
    }

    while (1) {
        c = sh_getchar();

        if (c == EOF) {
            _exit(EXIT_SUCCESS);
        } else if (c == '\n' || c == '\r') {
            buffer[position] = '\0';
            return buffer;
        } else {
            buffer[position] = (char)c;
        }
        position++;

        if (position >= bufsize) {
            bufsize += LSH_RL_BUFSIZE;
            buffer = sh_realloc(buffer, bufsize);
            if (!buffer) {
                sh_print(2, "lsh: allocation error\n");
                _exit(EXIT_FAILURE);
            }
        }
    }
}

void lsh_loop(void) {
    char *line;
    char **tokens;
    int status;

    do {
        sh_heap_pos = 0;

        sh_print(1, "# ");
        line = lsh_read_line();
        tokens = lsh_split_line(line);
        status = lsh_execute(tokens);

        sh_free(line);
        sh_free(tokens);
    } while (status);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    lsh_loop();

    return EXIT_SUCCESS;
}