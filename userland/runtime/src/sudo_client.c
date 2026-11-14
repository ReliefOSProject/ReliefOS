/* Compatibility API backed by the upstream executables and their sudoers/PAM policy. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <reliefos/sudo.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define ASKPASS "/usr/lib/reliefos/apps/reliefaskpass/reliefaskpass.elf"
#define FILEOP_WORKER "/usr/lib/reliefos/apps/sudod/sudod.elf"
#define RESULT_MAGIC 0x524f4453u
struct fileop_result {
    uint32_t magic;
    int32_t status;
    uint32_t count, reserved;
    struct reliefos_dir_entry entries[RELIEFOS_FS_MAX_ENTRIES];
};

static void sudo_clear_secret(char *text, uint32_t length)
{
    explicit_bzero(text, length);
}

static int private_pipe(int pair[2])
{
    if (pipe2(pair, O_CLOEXEC) < 0) return -1;
    for (unsigned i = 0; i < 2; ++i) {
        if (pair[i] >= 3) continue;
        int fd = fcntl(pair[i], F_DUPFD_CLOEXEC, 3);
        if (fd < 0) {
            int error = errno;
            close(pair[0]); close(pair[1]); errno = error;
            return -1;
        }
        close(pair[i]); pair[i] = fd;
    }
    return 0;
}

/* The error pipe closes at exec, so a failed exec cannot become a successful spawn. */
static int spawn(char *const args[], int output, uint32_t *pid_out)
{
    int errors[2];
    if (private_pipe(errors) < 0) return -1;
    pid_t pid = fork();
    if (!pid) {
        close(errors[0]);
        int error = 0;
        if (output >= 0 && dup2(output, STDOUT_FILENO) < 0) error = errno;
        int report = errors[1];
        if (report != 3) {
            if (dup3(report, 3, O_CLOEXEC) < 0) error = errno;
            else report = 3;
        }
        if (!error && syscall(SYS_close_range, 4u, ~0u, 0u) < 0) error = errno;
        if (!error && setenv("SUDO_ASKPASS", ASKPASS, 1) < 0) error = errno;
        if (!error) { execv(args[0], args); error = errno; }
        (void)write(report, &error, sizeof(error));
        _exit(127);
    }
    int error = errno;
    close(errors[1]);
    if (pid < 0) { close(errors[0]); errno = error; return -1; }
    ssize_t got;
    do { got = read(errors[0], &error, sizeof(error)); } while (got < 0 && errno == EINTR);
    close(errors[0]);
    if (got != 0) {
        int status;
        if (got < 0) { error = errno; kill(pid, SIGTERM); }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        errno = got == sizeof(error) || got < 0 ? error : EIO;
        return -1;
    }
    *pid_out = (uint32_t)pid;
    return 0;
}

static int run_as(const char *user, const char *password, char *const argv[],
                  unsigned mode, int output, uint32_t *pid)
{
    if (!argv || !argv[0] || !pid) { errno = EINVAL; return -1; }
    if (password && *password) { errno = ENOTSUP; return -1; }
    size_t count = 0;
    while (argv[count]) {
        if (count >= SIZE_MAX / sizeof(char *) - 10) { errno = E2BIG; return -1; }
        ++count;
    }
    char **args = calloc(count + 10, sizeof(*args));
    if (!args) return -1;
    size_t n = 0;
    args[n++] = mode ? "/bin/su" : "/usr/bin/sudo";
    if (!mode) {
        int askpass = output >= 0 || !isatty(STDIN_FILENO);
        args[n++] = askpass ? "-A" : "-u";
        if (askpass) args[n++] = "-u";
        args[n++] = (char *)(user && *user ? user : "root");
        args[n++] = "--";
        for (size_t i = 0; i < count; ++i) args[n++] = argv[i];
    } else {
        if (mode == 2) args[n++] = "--login";
        args[n++] = "--shell";
        args[n++] = argv[0];
        args[n++] = "--";
        args[n++] = (char *)(user && *user ? user : "root");
        for (size_t i = 1; i < count; ++i) args[n++] = argv[i];
    }
    int result = spawn(args, output, pid), error = errno;
    free(args); errno = error;
    return result;
}

int reliefos_sudo_run(const char *u, const char *p, char *const a[], uint32_t *pid)
{ return run_as(u, p, a, 0, -1, pid); }
int reliefos_sudo_run_stdout(const char *u, char *const a[], int output, uint32_t *pid)
{
    if (output < 0) { errno = EBADF; return -1; }
    return run_as(u, NULL, a, 0, output, pid);
}
int reliefos_sudo_run_switch(const char *u, const char *p, char *const a[], uint32_t *pid)
{ return run_as(u, p, a, 1, -1, pid); }
int reliefos_sudo_run_login(const char *u, const char *p, char *const a[], uint32_t *pid)
{ return run_as(u, p, a, 2, -1, pid); }

int reliefos_sudo_wait(uint32_t pid, int *status)
{
    if (!pid || !status) { errno = EINVAL; return -1; }
    pid_t result = waitpid((pid_t)pid, status, WNOHANG);
    if (!result) { errno = EAGAIN; return -1; }
    return result < 0 ? -1 : 0;
}
int reliefos_sudo_wait_command(uint32_t pid, int *status)
{
    if (!pid || !status) { errno = EINVAL; return -1; }
    pid_t result;
    do { result = waitpid((pid_t)pid, status, 0); } while (result < 0 && errno == EINTR);
    return result < 0 ? -1 : 0;
}

static int sudo_option(char *option, int noninteractive)
{
    char *args[] = {"/usr/bin/sudo", noninteractive ? "-n" : "-A", option, NULL};
    uint32_t pid; int status;
    if (spawn(args, -1, &pid) < 0 || reliefos_sudo_wait_command(pid, &status) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { errno = EACCES; return -1; }
    return 0;
}
int reliefos_sudo_check(void)
{
    /* This is a UI hint only. Every subsequent command invokes sudo again. */
    char *args[] = {"/usr/bin/sudo", "-n", "-N", "-v", NULL};
    uint32_t pid; int status;
    if (spawn(args, -1, &pid) < 0 || reliefos_sudo_wait_command(pid, &status) < 0) return -1;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 1 : 0;
}
int reliefos_sudo_verify(const char *user, const char *password)
{
    if ((user && *user) || (password && *password)) { errno = ENOTSUP; return -1; }
    return sudo_option("-v", 0);
}
int reliefos_sudo_kill(void) { return sudo_option("-k", 1); }

static volatile sig_atomic_t password_signal;

static void password_interrupted(int signal_number)
{
    password_signal = signal_number;
}

int reliefos_read_password(const char *prompt, char *buffer, uint32_t capacity)
{
    struct termios saved;
    struct termios current;
    const int signals[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGTSTP};
    struct sigaction previous[5], handler = {0};
    uint32_t used = 0;
    int installed = 0, error = 0, overflow = 0;

    if (!buffer || capacity < 2) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = 0;
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    if (tcgetattr(fd, &saved) < 0) { close(fd); return -1; }
    handler.sa_handler = password_interrupted;
    sigemptyset(&handler.sa_mask);
    password_signal = 0;
    for (; installed < 5; ++installed)
        if (sigaction(signals[installed], &handler, &previous[installed]) < 0) break;
    if (installed != 5) error = errno;
    current = saved;
    current.c_lflag &= (tcflag_t)~(ECHO | ECHONL);
    if (!error && tcsetattr(fd, TCSAFLUSH, &current) < 0) error = errno;
    if (!error && prompt && write(fd, prompt, strlen(prompt)) < 0) error = errno;
    while (!error && !password_signal) {
        char ch = 0;
        ssize_t got = read(fd, &ch, 1);
        if (got < 0) {
            if (errno == EINTR && !password_signal) {
                continue;
            }
            error = errno;
            break;
        }
        if (got == 0) {
            error = EIO;
            break;
        }
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if ((ch == 8 || ch == 127) && used) {
            --used;
            continue;
        }
        if (ch == 4 && !used) {
            break;
        }
        if (used + 1 < capacity) {
            buffer[used++] = ch;
        } else overflow = 1;
    }
    buffer[used] = 0;
    if (tcsetattr(fd, TCSAFLUSH, &saved) < 0) error = errno;
    (void)write(fd, "\n", 1);
    close(fd);
    for (int i = installed - 1; i >= 0; --i) sigaction(signals[i], &previous[i], NULL);
    if (overflow) error = EOVERFLOW;
    if (password_signal) error = EINTR;
    if (error) sudo_clear_secret(buffer, capacity);
    if (password_signal) raise(password_signal);
    errno = error;
    return error ? -1 : 0;
}


int reliefos_fileop(uint32_t op, const char *path1, const char *path2,
                  const char *username, const char *password,
                  struct reliefos_dir_entry *entries, uint32_t capacity,
                  uint32_t *out_count)
{
    if (!path1 || !*path1 || !out_count || (!entries && capacity) ||
        op < RELIEFOS_FILEOP_LIST || op > RELIEFOS_FILEOP_UNLINK) { errno = EINVAL; return -1; }
    *out_count = 0;
    if ((username && *username) || (password && *password)) { errno = ENOTSUP; return -1; }
    char verb[16];
    snprintf(verb, sizeof(verb), "%u", op);
    char *args[] = {"/usr/bin/sudo", "-A", "-u", "root", "--", FILEOP_WORKER,
                    "--op", verb, "--path1", (char *)path1, "--path2", (char *)(path2 ? path2 : ""), NULL};
    struct fileop_result *reply = calloc(1, sizeof(*reply));
    if (!reply) return -1;
    int pair[2], status = 0, error = 0;
    if (private_pipe(pair) < 0) { free(reply); return -1; }
    uint32_t pid;
    if (spawn(args, pair[1], &pid) < 0) {
        error = errno; close(pair[0]); close(pair[1]); free(reply); errno = error; return -1;
    }
    close(pair[1]);
    size_t length = 0;
    while (length < sizeof(*reply)) {
        ssize_t n = read(pair[0], (char *)reply + length, sizeof(*reply) - length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n < 0) error = errno; break; }
        length += (size_t)n;
    }
    close(pair[0]);
    if (reliefos_sudo_wait_command(pid, &status) < 0 && !error) error = errno;
    if (!error && (length != sizeof(*reply) || reply->magic != RESULT_MAGIC ||
                   reply->reserved || reply->count > RELIEFOS_FS_MAX_ENTRIES)) error = EPROTO;
    if (!error && reply->status) error = reply->status < 0 && reply->status >= -4095 ? -reply->status : EIO;
    if (!error && (!WIFEXITED(status) || WEXITSTATUS(status))) error = EACCES;
    if (!error) {
        uint32_t count = reply->count < capacity ? reply->count : capacity;
        for (uint32_t i = 0; i < count; ++i)
            if (!memchr(reply->entries[i].name, 0, sizeof(reply->entries[i].name))) error = EPROTO;
        if (!error) {
            if (count) memcpy(entries, reply->entries, count * sizeof(*entries));
            *out_count = count;
        }
    }
    free(reply); errno = error;
    return error ? -1 : 0;
}
/* Published libleonos.so.2 aliases; keep these in the defining translation unit. */
extern __typeof__(reliefos_fileop) leonos_fileop __attribute__((alias("reliefos_fileop")));
extern __typeof__(reliefos_read_password) leonos_read_password __attribute__((alias("reliefos_read_password")));
extern __typeof__(reliefos_sudo_check) leonos_sudo_check __attribute__((alias("reliefos_sudo_check")));
extern __typeof__(reliefos_sudo_kill) leonos_sudo_kill __attribute__((alias("reliefos_sudo_kill")));
extern __typeof__(reliefos_sudo_run) leonos_sudo_run __attribute__((alias("reliefos_sudo_run")));
extern __typeof__(reliefos_sudo_run_login) leonos_sudo_run_login __attribute__((alias("reliefos_sudo_run_login")));
extern __typeof__(reliefos_sudo_run_stdout) leonos_sudo_run_stdout __attribute__((alias("reliefos_sudo_run_stdout")));
extern __typeof__(reliefos_sudo_run_switch) leonos_sudo_run_switch __attribute__((alias("reliefos_sudo_run_switch")));
extern __typeof__(reliefos_sudo_verify) leonos_sudo_verify __attribute__((alias("reliefos_sudo_verify")));
extern __typeof__(reliefos_sudo_wait) leonos_sudo_wait __attribute__((alias("reliefos_sudo_wait")));
extern __typeof__(reliefos_sudo_wait_command) leonos_sudo_wait_command __attribute__((alias("reliefos_sudo_wait_command")));
