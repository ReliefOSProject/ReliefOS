#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern "C" char **environ;

namespace ra {
namespace {

constexpr size_t OutputLimit = 64u * 1024u * 1024u;
constexpr size_t ErrorLimit = 1024u * 1024u;
constexpr size_t PasswordLimit = 1024u;
constexpr size_t MaxArguments = 8192;
constexpr unsigned ReadChunk = 4096;
constexpr uint64_t DrainGraceMs = 200;
constexpr uint64_t KillGraceMs = 200;
constexpr uint64_t PostKillDrainMs = 75;
const char RootWorker[] = "/usr/lib/reliefos/apps/reliefapk/reliefapk.elf";
const char WorkerReadyMarker[] = "__RELIEFAPK_WORKER_READY__";
const char SuPasswordPrompt[] = "Password:";
char ChildLocaleAll[] = "LC_ALL=C";
char ChildLang[] = "LANG=C";

struct State {
    pid_t pid;
    pid_t process_group;
    int output_fd;
    int errors_fd;
    int exec_fd;
    int auth_fd;
    int pty_master;
    int pty_slave;
    int exit_status;
    int exec_errno;
    int active;
    int reaped;
    int wrapper;
    int auth_is_su;
    int auth_ready;
    int auth_prompt;
    int su_prompt_seen;
    int cancelable;
    int cancel_requested;
    int term_sent;
    int kill_sent;
    int stderr_parser_flushed;
    int truncated;
    int read_turn;
    uint64_t leader_exit_ms;
    uint64_t term_sent_ms;
    uint64_t kill_sent_ms;
    char *output_data;
    size_t output_length;
    size_t output_capacity;
    char *errors_data;
    size_t errors_length;
    size_t errors_capacity;
    char auth_marker[96];
    size_t auth_marker_length;
    char parse_pending[96];
    size_t parse_pending_length;
    char pty_tail[sizeof(SuPasswordPrompt)];
    size_t pty_tail_length;
    char exec_error_bytes[sizeof(int)];
    size_t exec_error_length;
    char password[PasswordLimit + 2];
    size_t password_length;
    size_t password_offset;
};

void initialize_state(State *state)
{
    memset(state, 0, sizeof(*state));
    state->pid = -1;
    state->process_group = -1;
    state->output_fd = -1;
    state->errors_fd = -1;
    state->exec_fd = -1;
    state->auth_fd = -1;
    state->pty_master = -1;
    state->pty_slave = -1;
    state->exit_status = -1;
    state->read_turn = 0;
}

void wipe_password(State *state)
{
    volatile char *bytes = state->password;
    for (size_t i = 0; i < sizeof(state->password); ++i) bytes[i] = 0;
    state->password_length = 0;
    state->password_offset = 0;
}

bool su_terminal_echo_disabled(const State *state)
{
    if (!state || !state->auth_is_su || state->auth_ready || state->pty_slave < 0)
        return false;
    struct termios attributes;
    return tcgetattr(state->pty_slave, &attributes) == 0 &&
        !(attributes.c_lflag & ECHO);
}

void refresh_su_auth_prompt(State *state)
{
    if (!state || !state->auth_is_su) return;
    state->auth_prompt = state->active && !state->auth_ready &&
        state->su_prompt_seen && su_terminal_echo_disabled(state);
}

void close_fd(int &fd)
{
    if (fd >= 0) {
        (void)close(fd);
        fd = -1;
    }
}

void close_streams(State *state)
{
    close_fd(state->output_fd);
    close_fd(state->errors_fd);
    close_fd(state->exec_fd);
    close_fd(state->pty_master);
    close_fd(state->pty_slave);
    close_fd(state->auth_fd);
}

void release_state(State *state)
{
    if (!state) return;
    close_streams(state);
    wipe_password(state);
    free(state->output_data);
    free(state->errors_data);
    free(state);
}

uint64_t monotonic_ms()
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000u +
        static_cast<uint64_t>(now.tv_nsec / 1000000u);
}

bool set_descriptor_flags(int fd, int command, int flag)
{
    int value = fcntl(fd, command == F_SETFD ? F_GETFD : F_GETFL);
    if (value < 0) return false;
    return fcntl(fd, command, value | flag) == 0;
}

bool make_pipe(int descriptors[2])
{
    if (pipe(descriptors) < 0) return false;
    if (!set_descriptor_flags(descriptors[0], F_SETFD, FD_CLOEXEC) ||
        !set_descriptor_flags(descriptors[1], F_SETFD, FD_CLOEXEC)) {
        int saved = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        descriptors[0] = descriptors[1] = -1;
        errno = saved;
        return false;
    }
    return true;
}

bool set_nonblocking(int fd)
{
    return set_descriptor_flags(fd, F_SETFL, O_NONBLOCK);
}

bool is_locale_environment_entry(const char *entry)
{
    return strncmp(entry, "LC_ALL=", 7) == 0 || strncmp(entry, "LANG=", 5) == 0;
}

char **make_child_environment()
{
    size_t count = 0;
    size_t max_entries = static_cast<size_t>(-1) / sizeof(char *) - 3;
    if (environ) {
        while (environ[count]) {
            if (count == max_entries) {
                errno = E2BIG;
                return nullptr;
            }
            ++count;
        }
    }

    size_t retained = 0;
    for (size_t i = 0; i < count; ++i)
        if (!is_locale_environment_entry(environ[i])) ++retained;
    if (retained > static_cast<size_t>(-1) / sizeof(char *) - 3) {
        errno = E2BIG;
        return nullptr;
    }

    char **child_environment = static_cast<char **>(
        malloc((retained + 3) * sizeof(char *)));
    if (!child_environment) return nullptr;

    size_t at = 0;
    for (size_t i = 0; i < count; ++i)
        if (!is_locale_environment_entry(environ[i])) child_environment[at++] = environ[i];
    child_environment[at++] = ChildLocaleAll;
    child_environment[at++] = ChildLang;
    child_environment[at] = nullptr;
    return child_environment;
}

void setup_failure(State *state, int error)
{
    close_streams(state);
    state->exec_errno = error;
    state->exit_status = 127;
    state->active = 0;
}

bool append_bytes(State *state, char *&buffer, size_t &length, size_t &capacity,
    size_t limit, const char *bytes, size_t count)
{
    if (!count) return true;
    size_t available = length < limit ? limit - length : 0;
    size_t take = count < available ? count : available;
    if (take < count) state->truncated = 1;
    if (!take) return true;

    size_t required = length + take + 1;
    if (capacity < required) {
        size_t grown = capacity ? capacity : 4096;
        while (grown < required) {
            size_t next = grown <= limit / 2 ? grown * 2 : limit + 1;
            if (next <= grown) { grown = required; break; }
            grown = next;
        }
        if (grown > limit + 1) grown = limit + 1;
        char *replacement = static_cast<char *>(realloc(buffer, grown));
        if (!replacement) {
            state->truncated = 1;
            return false;
        }
        buffer = replacement;
        capacity = grown;
    }
    memcpy(buffer + length, bytes, take);
    length += take;
    buffer[length] = '\0';
    return true;
}

void append_error(State *state, const char *bytes, size_t count)
{
    append_bytes(state, state->errors_data, state->errors_length,
        state->errors_capacity, ErrorLimit, bytes, count);
}

bool is_prefix(const char *candidate, size_t candidate_length, const char *marker)
{
    size_t marker_length = strlen(marker);
    return candidate_length <= marker_length &&
        memcmp(candidate, marker, candidate_length) == 0;
}

void authentication_ready(State *state)
{
    state->auth_ready = 1;
    state->auth_prompt = 0;
    state->su_prompt_seen = 0;
    wipe_password(state);
    close_fd(state->auth_fd);
    close_fd(state->pty_slave);
}

void stderr_marker(State *state, const char *marker, size_t length)
{
    if (!state->wrapper) return;
    if (state->auth_is_su && !state->auth_ready &&
        length == sizeof(SuPasswordPrompt) - 1 &&
        memcmp(marker, SuPasswordPrompt, length) == 0) {
        state->su_prompt_seen = 1;
        refresh_su_auth_prompt(state);
        return;
    }
    if (length == state->auth_marker_length &&
        memcmp(marker, state->auth_marker, length) == 0 && !state->auth_ready) {
        state->auth_prompt = 1;
        return;
    }
    if (length == sizeof(WorkerReadyMarker) - 1 &&
        memcmp(marker, WorkerReadyMarker, length) == 0) {
        authentication_ready(state);
    }
}

void parse_stderr_byte(State *state, char byte)
{
    if (state->parse_pending_length < sizeof(state->parse_pending))
        state->parse_pending[state->parse_pending_length++] = byte;
    else {
        append_error(state, state->parse_pending, 1);
        memmove(state->parse_pending, state->parse_pending + 1,
            --state->parse_pending_length);
        state->parse_pending[state->parse_pending_length++] = byte;
    }

    for (;;) {
        size_t count = state->parse_pending_length;
        int auth_exact = count == state->auth_marker_length &&
            memcmp(state->parse_pending, state->auth_marker, count) == 0;
        size_t ready_length = sizeof(WorkerReadyMarker) - 1;
        int ready_exact = count == ready_length &&
            memcmp(state->parse_pending, WorkerReadyMarker, count) == 0;
        size_t su_prompt_length = sizeof(SuPasswordPrompt) - 1;
        int su_prompt_exact = state->auth_is_su && !state->auth_ready &&
            count == su_prompt_length &&
            memcmp(state->parse_pending, SuPasswordPrompt, count) == 0;
        if (auth_exact || ready_exact || su_prompt_exact) {
            stderr_marker(state, state->parse_pending, count);
            state->parse_pending_length = 0;
            return;
        }

        int auth_prefix = is_prefix(state->parse_pending, count, state->auth_marker);
        int ready_prefix = is_prefix(state->parse_pending, count, WorkerReadyMarker);
        int su_prompt_prefix = state->auth_is_su && !state->auth_ready &&
            is_prefix(state->parse_pending, count, SuPasswordPrompt);
        if (auth_prefix || ready_prefix || su_prompt_prefix) return;

        append_error(state, state->parse_pending, 1);
        --state->parse_pending_length;
        if (state->parse_pending_length)
            memmove(state->parse_pending, state->parse_pending + 1,
                state->parse_pending_length);
        if (!state->parse_pending_length) return;
    }
}

void parse_stderr(State *state, const char *bytes, size_t count)
{
    if (!state->wrapper) {
        append_error(state, bytes, count);
        return;
    }
    for (size_t i = 0; i < count; ++i) parse_stderr_byte(state, bytes[i]);
}

void flush_stderr_parser(State *state)
{
    if (state->stderr_parser_flushed) return;
    if (state->parse_pending_length)
        append_error(state, state->parse_pending, state->parse_pending_length);
    state->parse_pending_length = 0;
    state->stderr_parser_flushed = 1;
}

void parse_su_pty(State *state, const char *bytes, size_t count)
{
    if (state->auth_ready) return;
    const size_t prompt_length = sizeof(SuPasswordPrompt) - 1;
    for (size_t i = 0; i < count; ++i) {
        char byte = bytes[i];
        if (state->pty_tail_length < prompt_length)
            state->pty_tail[state->pty_tail_length++] = byte;
        else {
            memmove(state->pty_tail, state->pty_tail + 1, prompt_length - 1);
            state->pty_tail[prompt_length - 1] = byte;
        }
        if (state->pty_tail_length == prompt_length &&
            memcmp(state->pty_tail, SuPasswordPrompt, prompt_length) == 0 &&
            !state->auth_ready) {
            state->su_prompt_seen = 1;
            refresh_su_auth_prompt(state);
        }
    }
}

ssize_t write_without_sigpipe(int fd, const char *bytes, size_t count)
{
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0) return -1;
    int had_pending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE);
    ssize_t result;
    do { result = write(fd, bytes, count); } while (result < 0 && errno == EINTR);
    int saved_error = errno;
    if (result < 0 && saved_error == EPIPE && !had_pending) {
        struct timespec immediate = {0, 0};
        while (sigtimedwait(&blocked, nullptr, &immediate) < 0 && errno == EINTR) {}
    }
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    errno = saved_error;
    return result;
}

void send_pending_password(State *state)
{
    if (!state->password_length || state->auth_ready) return;
    if (state->auth_is_su && !su_terminal_echo_disabled(state)) return;
    int fd = state->auth_is_su ? state->pty_master : state->auth_fd;
    if (fd < 0) {
        wipe_password(state);
        return;
    }
    while (state->password_offset < state->password_length) {
        const char *bytes = state->password + state->password_offset;
        size_t left = state->password_length - state->password_offset;
        ssize_t sent = state->auth_is_su ? write(fd, bytes, left) :
            write_without_sigpipe(fd, bytes, left);
        if (sent > 0) {
            volatile char *written = state->password + state->password_offset;
            for (ssize_t i = 0; i < sent; ++i) written[i] = 0;
            state->password_offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EINTR)) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        wipe_password(state);
        return;
    }
    wipe_password(state);
}

void terminate_group(State *state, int signal_number)
{
    if (state->process_group > 0) (void)kill(-state->process_group, signal_number);
}

void request_termination(State *state)
{
    if (!state->term_sent) {
        terminate_group(state, SIGTERM);
        state->term_sent = 1;
        state->cancel_requested = 1;
        state->term_sent_ms = monotonic_ms();
        state->auth_prompt = 0;
        state->su_prompt_seen = 0;
        wipe_password(state);
        close_fd(state->auth_fd);
    }
}

void reset_child_signals()
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGQUIT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    sigaction(SIGPIPE, &action, nullptr);
}

void child_failure(int fd, int error)
{
    const char *bytes = reinterpret_cast<const char *>(&error);
    size_t left = sizeof(error);
    while (left) {
        ssize_t wrote = write(fd, bytes, left);
        if (wrote > 0) {
            bytes += wrote;
            left -= static_cast<size_t>(wrote);
        } else if (wrote < 0 && errno == EINTR) continue;
        else break;
    }
    _exit(127);
}

size_t argument_count(const char *const argv[])
{
    if (!argv || !argv[0]) return 0;
    size_t count = 0;
    while (count < MaxArguments && argv[count]) ++count;
    if (count == MaxArguments) return 0;
    return count;
}

void fail_start(State *state, int error)
{
    setup_failure(state, error ? error : EIO);
}

void make_auth_marker(State *state)
{
    static unsigned long counter = 0;
    ++counter;
    snprintf(state->auth_marker, sizeof(state->auth_marker),
        "__RELIEFAPK_AUTH_%ld_%llu_%lu__", static_cast<long>(getpid()),
        static_cast<unsigned long long>(monotonic_ms()), counter);
    state->auth_marker_length = strlen(state->auth_marker);
}

char **make_wrapper_argv(State *state, const char *const original[], size_t count,
    AuthMode mode)
{
    size_t prefix = mode == AuthMode::Sudo ? 9 : 6;
    char **result = static_cast<char **>(calloc(count + prefix + 1, sizeof(char *)));
    if (!result) return nullptr;
    size_t at = 0;
    if (mode == AuthMode::Sudo) {
        make_auth_marker(state);
        result[at++] = const_cast<char *>("/usr/bin/sudo");
        result[at++] = const_cast<char *>("-S");
        result[at++] = const_cast<char *>("-p");
        result[at++] = state->auth_marker;
        result[at++] = const_cast<char *>("-u");
        result[at++] = const_cast<char *>("root");
        result[at++] = const_cast<char *>("--");
        result[at++] = const_cast<char *>(RootWorker);
        result[at++] = const_cast<char *>("--worker");
    } else {
        result[at++] = const_cast<char *>("/bin/su");
        result[at++] = const_cast<char *>("--shell");
        result[at++] = const_cast<char *>(RootWorker);
        result[at++] = const_cast<char *>("--");
        result[at++] = const_cast<char *>("root");
        result[at++] = const_cast<char *>("--worker");
    }
    for (size_t i = 0; i < count; ++i)
        result[at++] = const_cast<char *>(original[i]);
    result[at] = nullptr;
    return result;
}

bool open_su_pty(int &master, int &slave)
{
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return false;
    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        int saved = errno;
        close(master);
        master = -1;
        errno = saved;
        return false;
    }
    char *name = ptsname(master);
    if (!name) {
        int saved = errno;
        close(master);
        master = -1;
        errno = saved;
        return false;
    }
    slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        int saved = errno;
        close(master);
        master = -1;
        errno = saved;
        return false;
    }
    if (!set_nonblocking(master)) {
        int saved = errno;
        close(master);
        close(slave);
        master = slave = -1;
        errno = saved;
        return false;
    }
    return true;
}

void child_exec(State *state, const char *const original[], char *const exec_argv[],
    bool wrapped, int out_pipe[2], int err_pipe[2], int exec_pipe[2],
    int auth_pipe[2], int pty_master, int pty_slave, char *const child_environment[])
{
    close(out_pipe[0]);
    close(err_pipe[0]);
    close(exec_pipe[0]);
    if (auth_pipe[1] >= 0 && state->auth_is_su) close(auth_pipe[1]);

    if (state->auth_is_su) {
        close(pty_master);
        if (setsid() < 0) child_failure(exec_pipe[1], errno);
        if (ioctl(pty_slave, TIOCSCTTY, 0) < 0) child_failure(exec_pipe[1], errno);
        if (dup2(pty_slave, STDIN_FILENO) < 0) child_failure(exec_pipe[1], errno);
    } else if (wrapped) {
        if (dup2(auth_pipe[0], STDIN_FILENO) < 0) child_failure(exec_pipe[1], errno);
        close(auth_pipe[0]);
    } else {
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd < 0) child_failure(exec_pipe[1], errno);
        if (dup2(null_fd, STDIN_FILENO) < 0) child_failure(exec_pipe[1], errno);
        if (null_fd != STDIN_FILENO) close(null_fd);
    }

    if (dup2(out_pipe[1], STDOUT_FILENO) < 0) child_failure(exec_pipe[1], errno);
    if (dup2(err_pipe[1], STDERR_FILENO) < 0) child_failure(exec_pipe[1], errno);

    if (!state->auth_is_su && wrapped) close(auth_pipe[1]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pty_slave >= 0 && pty_slave != STDIN_FILENO) close(pty_slave);
    reset_child_signals();
    if (!state->auth_is_su) {
        if (setpgid(0, 0) < 0 && errno != EACCES) child_failure(exec_pipe[1], errno);
    }

    const char *path = wrapped ? exec_argv[0] : original[0];
    execve(path, exec_argv, child_environment);
    child_failure(exec_pipe[1], errno);
}

void record_wait_status(State *state, int status)
{
    if (WIFEXITED(status)) state->exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) state->exit_status = 128 + WTERMSIG(status);
    else state->exit_status = 127;
    state->reaped = 1;
    state->leader_exit_ms = monotonic_ms();
    close_fd(state->auth_fd);
    if (!state->auth_ready) {
        close_fd(state->pty_master);
        close_fd(state->pty_slave);
        state->auth_prompt = 0;
    }
    wipe_password(state);
}

void poll_child(State *state)
{
    if (state->reaped || state->pid <= 0) return;
    int status = 0;
    pid_t result;
    do { result = waitpid(state->pid, &status, WNOHANG); }
    while (result < 0 && errno == EINTR);
    if (result == state->pid) record_wait_status(state, status);
    else if (result < 0 && errno == ECHILD) {
        state->exec_errno = state->exec_errno ? state->exec_errno : ECHILD;
        state->exit_status = 127;
        state->reaped = 1;
        state->leader_exit_ms = monotonic_ms();
        close_fd(state->auth_fd);
        close_fd(state->pty_master);
        close_fd(state->pty_slave);
        wipe_password(state);
    }
}

void read_exec_status(State *state, unsigned &budget)
{
    if (state->exec_fd < 0 || !budget) return;
    size_t left = sizeof(int) - state->exec_error_length;
    size_t take = left < budget ? left : budget;
    ssize_t result;
    do { result = read(state->exec_fd, state->exec_error_bytes + state->exec_error_length, take); }
    while (result < 0 && errno == EINTR);
    if (result > 0) {
        state->exec_error_length += static_cast<size_t>(result);
        budget -= static_cast<unsigned>(result);
        if (state->exec_error_length == sizeof(int)) {
            memcpy(&state->exec_errno, state->exec_error_bytes, sizeof(int));
            close_fd(state->exec_fd);
        }
    } else if (result == 0) {
        if (state->exec_error_length && !state->exec_errno) state->exec_errno = EIO;
        close_fd(state->exec_fd);
    }
    else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        state->exec_errno = errno;
        close_fd(state->exec_fd);
    }
}

void close_finished_stream(State *state, int slot)
{
    if (slot == 0) close_fd(state->output_fd);
    else if (slot == 1) {
        close_fd(state->errors_fd);
        flush_stderr_parser(state);
    } else close_fd(state->pty_master);
}

int stream_fd(const State *state, int slot)
{
    if (slot == 0) return state->output_fd;
    if (slot == 1) return state->errors_fd;
    return state->pty_master;
}

void read_one_stream(State *state, int slot, unsigned &budget, bool &progressed,
    bool &blocked)
{
    int fd = stream_fd(state, slot);
    if (fd < 0 || !budget || blocked) return;
    char bytes[ReadChunk];
    size_t take = budget < sizeof(bytes) ? budget : sizeof(bytes);
    ssize_t result;
    do { result = read(fd, bytes, take); } while (result < 0 && errno == EINTR);
    if (result > 0) {
        size_t count = static_cast<size_t>(result);
        budget -= static_cast<unsigned>(count);
        progressed = true;
        if (slot == 0)
            append_bytes(state, state->output_data, state->output_length,
                state->output_capacity, OutputLimit, bytes, count);
        else if (slot == 1) parse_stderr(state, bytes, count);
        else parse_su_pty(state, bytes, count);
    } else if (result == 0) close_finished_stream(state, slot);
    else if (errno == EAGAIN || errno == EWOULDBLOCK) blocked = true;
    else close_finished_stream(state, slot);
}

bool streams_closed(const State *state)
{
    return state->output_fd < 0 && state->errors_fd < 0 && state->exec_fd < 0 &&
        state->pty_master < 0;
}

bool process_group_exists(const State *state)
{
    if (state->process_group <= 0) return false;
    if (kill(-state->process_group, 0) == 0) return true;
    return errno == EPERM;
}

void maintenance(State *state)
{
    uint64_t now = monotonic_ms();
    if (state->reaped && !state->term_sent &&
        (!streams_closed(state) || process_group_exists(state)) &&
        now >= state->leader_exit_ms && now - state->leader_exit_ms >= DrainGraceMs)
        request_termination(state);
    if (state->term_sent && !state->kill_sent && now >= state->term_sent_ms &&
        now - state->term_sent_ms >= KillGraceMs) {
        if (process_group_exists(state)) terminate_group(state, SIGKILL);
        state->kill_sent = 1;
        state->kill_sent_ms = now;
    }
    if (state->kill_sent && now >= state->kill_sent_ms &&
        now - state->kill_sent_ms >= PostKillDrainMs && !streams_closed(state)) {
        close_fd(state->output_fd);
        close_fd(state->errors_fd);
        flush_stderr_parser(state);
        close_fd(state->exec_fd);
        close_fd(state->pty_master);
        close_fd(state->pty_slave);
    }
    bool process_cleanup_done = !state->term_sent && !process_group_exists(state);
    if (state->term_sent && state->kill_sent && now >= state->kill_sent_ms &&
        now - state->kill_sent_ms >= PostKillDrainMs) process_cleanup_done = true;
    if (state->reaped && streams_closed(state) && process_cleanup_done) {
        state->active = 0;
        state->pid = -1;
        state->process_group = -1;
        close_fd(state->auth_fd);
        close_fd(state->pty_slave);
        wipe_password(state);
    }
}

} // namespace

Process::Process() : state_(calloc(1, sizeof(State)))
{
    if (state_) initialize_state(static_cast<State *>(state_));
}

Process::~Process()
{
    State *state = static_cast<State *>(state_);
    if (!state) return;
    if (state->active && state->pid > 0) {
        terminate_group(state, SIGTERM);
        struct timespec grace = {0, 100000000L};
        while (nanosleep(&grace, &grace) < 0 && errno == EINTR) {}
        terminate_group(state, SIGKILL);
        close_streams(state);
        int status = 0;
        pid_t result;
        do { result = waitpid(state->pid, &status, 0); }
        while (result < 0 && errno == EINTR);
        wipe_password(state);
    }
    release_state(state);
    state_ = nullptr;
}

bool Process::start(const char *const argv[], bool privileged, AuthMode mode, bool cancelable)
{
    State *state = static_cast<State *>(state_);
    if (!state) return false;
    if (state->active) return false;

    close_streams(state);
    wipe_password(state);
    free(state->output_data);
    free(state->errors_data);
    initialize_state(state);

    size_t count = argument_count(argv);
    if (!count) {
        fail_start(state, EINVAL);
        return false;
    }

    bool wrapped = privileged && geteuid() != 0;
    bool use_su = wrapped && mode == AuthMode::RootPassword;
    state->wrapper = wrapped ? 1 : 0;
    state->auth_is_su = use_su ? 1 : 0;
    state->auth_ready = wrapped ? 0 : 1;
    state->cancelable = cancelable ? 1 : 0;
    char **wrapped_argv = nullptr;
    char *const *exec_argv = const_cast<char *const *>(argv);
    if (wrapped) {
        wrapped_argv = make_wrapper_argv(state, argv, count, mode);
        if (!wrapped_argv) {
            fail_start(state, ENOMEM);
            return false;
        }
        exec_argv = wrapped_argv;
    }

    char **child_environment = make_child_environment();
    if (!child_environment) {
        int saved = errno;
        free(wrapped_argv);
        fail_start(state, saved ? saved : ENOMEM);
        return false;
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    int auth_pipe[2] = {-1, -1};
    int pty_master = -1, pty_slave = -1;
    int error = 0;

    if (!make_pipe(out_pipe) || !make_pipe(err_pipe) || !make_pipe(exec_pipe)) error = errno;
    if (!error && wrapped && !use_su && !make_pipe(auth_pipe)) error = errno;
    if (!error && use_su && !open_su_pty(pty_master, pty_slave)) error = errno;
    if (!error && (!set_nonblocking(out_pipe[0]) || !set_nonblocking(err_pipe[0]) ||
        !set_nonblocking(exec_pipe[0]))) error = errno;
    if (!error && wrapped && !use_su && !set_nonblocking(auth_pipe[1])) error = errno;

    if (error) {
        for (int i = 0; i < 2; ++i) {
            if (out_pipe[i] >= 0) close(out_pipe[i]);
            if (err_pipe[i] >= 0) close(err_pipe[i]);
            if (exec_pipe[i] >= 0) close(exec_pipe[i]);
            if (auth_pipe[i] >= 0) close(auth_pipe[i]);
        }
        if (pty_master >= 0) close(pty_master);
        if (pty_slave >= 0) close(pty_slave);
        free(child_environment);
        free(wrapped_argv);
        fail_start(state, error);
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        error = errno;
        for (int i = 0; i < 2; ++i) {
            if (out_pipe[i] >= 0) close(out_pipe[i]);
            if (err_pipe[i] >= 0) close(err_pipe[i]);
            if (exec_pipe[i] >= 0) close(exec_pipe[i]);
            if (auth_pipe[i] >= 0) close(auth_pipe[i]);
        }
        if (pty_master >= 0) close(pty_master);
        if (pty_slave >= 0) close(pty_slave);
        free(child_environment);
        free(wrapped_argv);
        fail_start(state, error);
        return false;
    }
    if (child == 0)
        child_exec(state, argv, exec_argv, wrapped, out_pipe, err_pipe, exec_pipe,
            auth_pipe, pty_master, pty_slave, child_environment);

    state->pid = child;
    state->process_group = child;
    state->active = 1;
    state->output_fd = out_pipe[0];
    state->errors_fd = err_pipe[0];
    state->exec_fd = exec_pipe[0];
    state->auth_fd = wrapped && !use_su ? auth_pipe[1] : -1;
    state->pty_master = use_su ? pty_master : -1;
    state->pty_slave = use_su ? pty_slave : -1;

    close(out_pipe[1]);
    close(err_pipe[1]);
    close(exec_pipe[1]);
    if (auth_pipe[0] >= 0) close(auth_pipe[0]);
    if (use_su) {
        /* The child creates a session before attaching the slave as its tty. */
    } else {
        (void)setpgid(child, child);
    }
    free(child_environment);
    free(wrapped_argv);
    return true;
}

bool Process::step(unsigned budget)
{
    State *state = static_cast<State *>(state_);
    if (!state || !state->active) return false;

    refresh_su_auth_prompt(state);
    send_pending_password(state);
    unsigned remaining = budget;
    read_exec_status(state, remaining);

    int blocked[3] = {0, 0, 0};
    bool any_progress = true;
    while (remaining && any_progress) {
        any_progress = false;
        int first_slot = state->read_turn;
        for (int offset = 0; offset < 3 && remaining; ++offset) {
            int slot = (first_slot + offset) % 3;
            bool progressed = false;
            bool is_blocked = blocked[slot] != 0;
            read_one_stream(state, slot, remaining, progressed, is_blocked);
            if (is_blocked) blocked[slot] = 1;
            if (progressed) {
                any_progress = true;
                state->read_turn = (slot + 1) % 3;
            }
        }
    }

    refresh_su_auth_prompt(state);
    send_pending_password(state);
    poll_child(state);
    maintenance(state);
    return state->active != 0;
}

bool Process::running() const
{
    const State *state = static_cast<const State *>(state_);
    return state && state->active;
}

int Process::exit_code() const
{
    const State *state = static_cast<const State *>(state_);
    return state ? state->exit_status : -1;
}

int Process::exec_error() const
{
    const State *state = static_cast<const State *>(state_);
    return state ? state->exec_errno : ENOMEM;
}

const char *Process::output() const
{
    const State *state = static_cast<const State *>(state_);
    return state && state->output_data ? state->output_data : "";
}

size_t Process::output_size() const
{
    const State *state = static_cast<const State *>(state_);
    return state ? state->output_length : 0;
}

const char *Process::errors() const
{
    const State *state = static_cast<const State *>(state_);
    return state && state->errors_data ? state->errors_data : "";
}

size_t Process::errors_size() const
{
    const State *state = static_cast<const State *>(state_);
    return state ? state->errors_length : 0;
}

bool Process::truncated() const
{
    const State *state = static_cast<const State *>(state_);
    return state && state->truncated;
}

bool Process::password_requested() const
{
    const State *state = static_cast<const State *>(state_);
    return state && state->active && state->auth_prompt &&
        (!state->auth_is_su || su_terminal_echo_disabled(state));
}

namespace {

bool prepare_password(State *state, const char *secret)
{
    if (!state || !state->active || !secret || state->password_length) return false;
    refresh_su_auth_prompt(state);
    if (!state->auth_prompt ||
        (state->auth_is_su && !su_terminal_echo_disabled(state))) return false;
    size_t length = 0;
    while (length <= PasswordLimit && secret[length]) {
        if (secret[length] == '\r' || secret[length] == '\n') return false;
        ++length;
    }
    if (length > PasswordLimit) return false;
    memcpy(state->password, secret, length);
    state->password[length] = '\n';
    state->password[length + 1] = '\0';
    state->password_length = length + 1;
    state->password_offset = 0;
    state->auth_prompt = 0;
    if (state->auth_is_su) state->su_prompt_seen = 0;
    return true;
}

} // namespace

bool Process::provide_password(const char *secret)
{
    return prepare_password(static_cast<State *>(state_), secret);
}

void Process::cancel_auth()
{
    State *state = static_cast<State *>(state_);
    if (!state || !state->active || !state->wrapper || state->auth_ready) return;
    request_termination(state);
}

bool Process::cancel()
{
    State *state = static_cast<State *>(state_);
    if (!state || !state->active || !state->cancelable) return false;
    request_termination(state);
    return true;
}

} // namespace ra
