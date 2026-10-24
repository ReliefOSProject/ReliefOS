#include "process.h"
#include "process.cpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace {

const char *self_path = nullptr;
int checks = 0;
int failures = 0;
volatile sig_atomic_t pty_helper_saw_hup = 0;
constexpr size_t DualStreamPrebuffer = 4096;

void pty_helper_sighup(int)
{
    pty_helper_saw_hup = 1;
}

void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

void sleep_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

double now_seconds()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

bool write_all(int fd, const char *bytes, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, bytes + written, length - written);
        if (result > 0) written += static_cast<size_t>(result);
        else if (result < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool read_exact_with_timeout(int fd, char *bytes, size_t length, int timeout_ms)
{
    size_t read_length = 0;
    while (read_length < length) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        int ready;
        do { ready = poll(&descriptor, 1, timeout_ms); }
        while (ready < 0 && errno == EINTR);
        if (ready <= 0 || !(descriptor.revents & (POLLIN | POLLHUP))) return false;
        ssize_t result;
        do { result = read(fd, bytes + read_length, length - read_length); }
        while (result < 0 && errno == EINTR);
        if (result <= 0) return false;
        read_length += static_cast<size_t>(result);
    }
    return true;
}

bool append_marker(const char *path, char marker)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;
    bool written = write_all(fd, &marker, 1);
    close(fd);
    return written;
}

void wait_for_release_file(const char *path)
{
    while (access(path, F_OK) < 0) sleep_ms(1);
}

int child_main(int argc, char **argv)
{
    if (argc < 2) return 90;
    if (strcmp(argv[1], "--child-locale") == 0) {
        const char *lc_all = getenv("LC_ALL");
        const char *lang = getenv("LANG");
        char values[256];
        int length = snprintf(values, sizeof(values), "%s|%s\n",
            lc_all ? lc_all : "<unset>", lang ? lang : "<unset>");
        if (length < 0 || static_cast<size_t>(length) >= sizeof(values)) return 101;
        return write_all(STDOUT_FILENO, values, static_cast<size_t>(length)) ? 0 : 102;
    }
    if (strcmp(argv[1], "--child-streams") == 0) {
        if (!write_all(STDOUT_FILENO, "alpha\n", 6) ||
            !write_all(STDERR_FILENO, "beta\n", 5)) return 91;
        return 7;
    }
    if (strcmp(argv[1], "--child-slow") == 0) {
        for (int i = 0; i < 5; ++i) {
            sleep_ms(35);
            if (!write_all(STDOUT_FILENO, "x", 1)) return 92;
        }
        return 0;
    }
    if (strcmp(argv[1], "--child-wait") == 0) {
        for (;;) pause();
    }
    if (strcmp(argv[1], "--child-flood") == 0) {
        static char bytes[65536];
        memset(bytes, 'f', sizeof(bytes));
        const size_t out_length = 64u * 1024u * 1024u + 4097u;
        const size_t err_length = 1024u * 1024u + 4097u;
        size_t done = 0;
        while (done < out_length) {
            size_t count = out_length - done;
            if (count > sizeof(bytes)) count = sizeof(bytes);
            if (!write_all(STDOUT_FILENO, bytes, count)) return 93;
            done += count;
        }
        done = 0;
        while (done < err_length) {
            size_t count = err_length - done;
            if (count > sizeof(bytes)) count = sizeof(bytes);
            if (!write_all(STDERR_FILENO, bytes, count)) return 94;
            done += count;
        }
        return 0;
    }
    if (strcmp(argv[1], "--child-dual-flood") == 0 && argc > 3) {
        static char bytes[65536];
        memset(bytes, 'd', sizeof(bytes));
        pid_t writer = fork();
        if (writer < 0) return 97;
        if (writer == 0) {
            if (!write_all(STDOUT_FILENO, bytes, DualStreamPrebuffer) ||
                !append_marker(argv[2], 'O')) _exit(98);
            wait_for_release_file(argv[3]);
            size_t done = DualStreamPrebuffer;
            while (done < 256u * 1024u) {
                size_t count = 256u * 1024u - done;
                if (count > sizeof(bytes)) count = sizeof(bytes);
                if (!write_all(STDOUT_FILENO, bytes, count)) _exit(98);
                done += count;
            }
            _exit(0);
        }
        if (!write_all(STDERR_FILENO, bytes, DualStreamPrebuffer) ||
            !append_marker(argv[2], 'E')) return 99;
        wait_for_release_file(argv[3]);
        size_t done = DualStreamPrebuffer;
        while (done < 256u * 1024u) {
            size_t count = 256u * 1024u - done;
            if (count > sizeof(bytes)) count = sizeof(bytes);
            if (!write_all(STDERR_FILENO, bytes, count)) return 99;
            done += count;
        }
        int status = 0;
        pid_t waited;
        do { waited = waitpid(writer, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != writer) return 100;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 100;
    }
    if ((strcmp(argv[1], "--child-orphan") == 0 ||
         strcmp(argv[1], "--child-orphan-closed") == 0) && argc > 2) {
        int closed_streams = strcmp(argv[1], "--child-orphan-closed") == 0;
        pid_t descendant = fork();
        if (descendant < 0) return 95;
        if (descendant == 0) {
            if (closed_streams) {
                close(STDOUT_FILENO);
                close(STDERR_FILENO);
            }
            signal(SIGTERM, SIG_IGN);
            for (;;) {
                int fd = open(argv[2], O_WRONLY | O_CREAT | O_APPEND, 0600);
                if (fd >= 0) {
                    (void)write(fd, ".", 1);
                    close(fd);
                }
                sleep_ms(15);
            }
        }
        return 0;
    }
    return 96;
}

bool finish(ra::Process &process, unsigned budget = 65536, int max_steps = 12000)
{
    for (int i = 0; i < max_steps && process.running(); ++i) {
        process.step(budget);
        sleep_ms(1);
    }
    return !process.running();
}

void test_streams_exit_and_budget()
{
    ra::Process process;
    const char *argv[] = {self_path, "--child-streams", nullptr};
    const double before_start = now_seconds();
    check(process.start(argv), "start a real helper process");
    check(now_seconds() - before_start < 0.5, "start returns without waiting for the child");
    check(process.running() && process.exit_code() == -1, "running process has no exit code yet");
    sleep_ms(25);
    const size_t before_bytes = process.output_size() + process.errors_size();
    process.step(3);
    const size_t after_bytes = process.output_size() + process.errors_size();
    check(after_bytes - before_bytes <= 3, "step respects one shared stdout/stderr read budget");
    check(finish(process), "streams helper completes");
    check(process.exit_code() == 7, "normal exit status is preserved");
    check(process.exec_error() == 0, "successful exec has no exec error");
    check(process.output_size() == 6 && memcmp(process.output(), "alpha\n", 6) == 0,
        "stdout is captured separately");
    check(process.errors_size() == 5 && memcmp(process.errors(), "beta\n", 5) == 0,
        "stderr is captured separately");
}

void test_slow_process_is_pollable()
{
    ra::Process process;
    const char *argv[] = {self_path, "--child-slow", nullptr};
    const double before = now_seconds();
    check(process.start(argv), "start slow helper");
    process.step(64);
    check(now_seconds() - before < 0.25, "step stays responsive while child is sleeping");
    check(finish(process), "slow helper eventually completes");
    check(process.output_size() == 5, "slow output is drained while polling");
}

void test_child_locale_is_normalized_without_changing_parent()
{
    const char *current_lc_all = getenv("LC_ALL");
    const char *current_lang = getenv("LANG");
    char *saved_lc_all = current_lc_all ? strdup(current_lc_all) : nullptr;
    char *saved_lang = current_lang ? strdup(current_lang) : nullptr;
    bool snapshot_ok = (!current_lc_all || saved_lc_all) && (!current_lang || saved_lang);
    check(snapshot_ok, "save parent locale environment");
    if (!snapshot_ok) {
        free(saved_lc_all);
        free(saved_lang);
        return;
    }

    bool forced = setenv("LC_ALL", "zh_CN.UTF-8", 1) == 0 &&
        setenv("LANG", "zh_CN.UTF-8", 1) == 0;
    check(forced, "set a non-C parent locale for the subprocess test");
    if (forced) {
        ra::Process process;
        const char *argv[] = {self_path, "--child-locale", nullptr};
        check(process.start(argv), "start a real child under the parent GUI locale");
        check(finish(process), "locale helper child completes");
        check(process.output_size() == 4 && memcmp(process.output(), "C|C\n", 4) == 0,
            "child receives LC_ALL=C and LANG=C");
        check(getenv("LC_ALL") && strcmp(getenv("LC_ALL"), "zh_CN.UTF-8") == 0 &&
            getenv("LANG") && strcmp(getenv("LANG"), "zh_CN.UTF-8") == 0,
            "launching a child leaves the parent GUI locale unchanged");
    }

    bool restored_lc_all = saved_lc_all ? setenv("LC_ALL", saved_lc_all, 1) == 0 :
        unsetenv("LC_ALL") == 0;
    bool restored_lang = saved_lang ? setenv("LANG", saved_lang, 1) == 0 :
        unsetenv("LANG") == 0;
    check(restored_lc_all && restored_lang, "restore parent locale environment");
    free(saved_lc_all);
    free(saved_lang);
}

void test_authentication_ready_keeps_su_controlling_pty()
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    check(master >= 0, "create PTY master for ready-marker lifecycle test");
    if (master < 0) return;
    bool prepared = grantpt(master) == 0 && unlockpt(master) == 0;
    check(prepared, "prepare real PTY pair");
    if (!prepared) {
        close(master);
        return;
    }
    char *slave_name = ptsname(master);
    int slave = slave_name ? open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
    check(slave >= 0, "open PTY slave for controlling-terminal helper");
    if (slave < 0) {
        close(master);
        return;
    }

    int marker_pipe[2] = {-1, -1};
    check(pipe(marker_pipe) == 0, "create helper marker pipe");
    if (marker_pipe[0] < 0 || marker_pipe[1] < 0) {
        if (marker_pipe[0] >= 0) close(marker_pipe[0]);
        if (marker_pipe[1] >= 0) close(marker_pipe[1]);
        close(master);
        close(slave);
        return;
    }

    pty_helper_saw_hup = 0;
    pid_t helper = fork();
    check(helper >= 0, "fork a real PTY worker helper");
    if (helper < 0) {
        close(marker_pipe[0]);
        close(marker_pipe[1]);
        close(master);
        close(slave);
        return;
    }
    if (helper == 0) {
        close(master);
        close(marker_pipe[0]);
        if (dup2(marker_pipe[1], STDERR_FILENO) < 0) _exit(110);
        if (marker_pipe[1] != STDERR_FILENO) close(marker_pipe[1]);
        struct sigaction action = {};
        action.sa_handler = pty_helper_sighup;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGHUP, &action, nullptr) < 0) _exit(111);
        if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0) _exit(112);
        if (tcsetpgrp(slave, getpgrp()) < 0) _exit(113);
        const char marker[] = "__RELIEFAPK_WORKER_READY__\n";
        if (!write_all(STDERR_FILENO, marker, sizeof(marker) - 1)) _exit(114);
        sleep_ms(180);
        char result = pty_helper_saw_hup ? 'H' : 'A';
        if (!write_all(STDERR_FILENO, &result, 1)) _exit(115);
        _exit(pty_helper_saw_hup ? 1 : 0);
    }

    close(marker_pipe[1]);
    const char marker[] = "__RELIEFAPK_WORKER_READY__\n";
    char received_marker[sizeof(marker) - 1];
    bool marker_received = read_exact_with_timeout(marker_pipe[0], received_marker,
        sizeof(received_marker), 2000);
    check(marker_received && memcmp(received_marker, marker, sizeof(received_marker)) == 0,
        "PTY helper emits the worker-ready marker after taking its controlling terminal");

    if (marker_received) {
        ra::State state;
        ra::initialize_state(&state);
        state.wrapper = 1;
        state.auth_is_su = 1;
        state.auth_ready = 0;
        state.auth_prompt = 1;
        state.pty_master = master;
        state.pty_slave = slave;
        ra::authentication_ready(&state);
        check(state.auth_ready && !state.auth_prompt,
            "ready marker ends authentication prompt handling");
        const char post_auth_prompt[] = "Password:";
        ra::parse_su_pty(&state, post_auth_prompt, sizeof(post_auth_prompt) - 1);
        check(state.pty_tail_length == 0 && !state.auth_prompt,
            "PTY bytes after readiness are drained without further prompt parsing");
        check(state.pty_master == master && fcntl(master, F_GETFD) >= 0,
            "ready marker keeps PTY master alive for the authenticated worker");
        check(state.pty_slave == -1, "parent releases its PTY slave descriptor after auth");
        char result = 0;
        bool result_received = read_exact_with_timeout(marker_pipe[0], &result, 1, 2000);
        check(result_received && result == 'A',
            "authenticated controlling-terminal helper survives ready-marker handling");
        if (state.pty_master >= 0) ra::close_fd(state.pty_master);
        if (state.pty_slave >= 0) ra::close_fd(state.pty_slave);
    } else {
        close(master);
        close(slave);
    }

    int status = 0;
    pid_t waited;
    do { waited = waitpid(helper, &status, 0); } while (waited < 0 && errno == EINTR);
    check(waited == helper && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "reap PTY helper cleanly");
    close(marker_pipe[0]);
}

void test_su_stderr_prompt_uses_real_pty_echo_state()
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    check(master >= 0, "create PTY master for stderr authentication prompt");
    if (master < 0) return;
    bool prepared = grantpt(master) == 0 && unlockpt(master) == 0;
    check(prepared, "prepare PTY for stderr authentication prompt");
    if (!prepared) {
        close(master);
        return;
    }
    char *slave_name = ptsname(master);
    int slave = slave_name ? open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
    check(slave >= 0, "open controlling PTY for stderr authentication prompt");
    if (slave < 0) {
        close(master);
        return;
    }

    int errors_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    int echo_toggle_pipe[2] = {-1, -1};
    bool pipes_ready = pipe(errors_pipe) == 0 && pipe(result_pipe) == 0 &&
        pipe(echo_toggle_pipe) == 0;
    check(pipes_ready, "create separate stderr and helper result pipes");
    if (!pipes_ready) {
        if (errors_pipe[0] >= 0) close(errors_pipe[0]);
        if (errors_pipe[1] >= 0) close(errors_pipe[1]);
        if (result_pipe[0] >= 0) close(result_pipe[0]);
        if (result_pipe[1] >= 0) close(result_pipe[1]);
        if (echo_toggle_pipe[0] >= 0) close(echo_toggle_pipe[0]);
        if (echo_toggle_pipe[1] >= 0) close(echo_toggle_pipe[1]);
        close(master);
        close(slave);
        return;
    }

    pty_helper_saw_hup = 0;
    pid_t helper = fork();
    check(helper >= 0, "fork helper that authenticates through a controlling PTY");
    if (helper < 0) {
        close(errors_pipe[0]);
        close(errors_pipe[1]);
        close(result_pipe[0]);
        close(result_pipe[1]);
        close(echo_toggle_pipe[0]);
        close(echo_toggle_pipe[1]);
        close(master);
        close(slave);
        return;
    }
    if (helper == 0) {
        close(master);
        close(errors_pipe[0]);
        close(result_pipe[0]);
        close(echo_toggle_pipe[1]);
        if (dup2(errors_pipe[1], STDERR_FILENO) < 0) _exit(120);
        if (dup2(slave, STDIN_FILENO) < 0) _exit(121);
        if (errors_pipe[1] != STDERR_FILENO) close(errors_pipe[1]);
        if (slave != STDIN_FILENO) close(slave);
        if (setsid() < 0 || ioctl(STDIN_FILENO, TIOCSCTTY, 0) < 0) _exit(122);
        if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0) _exit(123);
        struct sigaction action = {};
        action.sa_handler = pty_helper_sighup;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGHUP, &action, nullptr) < 0) _exit(124);
        struct termios attributes;
        if (tcgetattr(STDIN_FILENO, &attributes) < 0) _exit(125);
        attributes.c_lflag |= ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) < 0) _exit(126);
        const char prompt[] = "Password:";
        if (!write_all(STDERR_FILENO, prompt, sizeof(prompt) - 1)) _exit(127);

        char toggle = 0;
        ssize_t toggle_count;
        do { toggle_count = read(echo_toggle_pipe[0], &toggle, 1); }
        while (toggle_count < 0 && errno == EINTR);
        if (toggle_count != 1 || toggle != 'E') _exit(132);
        if (tcgetattr(STDIN_FILENO, &attributes) < 0) _exit(133);
        attributes.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) < 0) _exit(134);
        const char echo_off = 'E';
        if (!write_all(result_pipe[1], &echo_off, 1)) _exit(135);

        const char expected[] = "pty-test-secret\n";
        char supplied[sizeof(expected)] = {};
        size_t supplied_length = 0;
        while (supplied_length < sizeof(expected) - 1) {
            ssize_t count = read(STDIN_FILENO, supplied + supplied_length,
                sizeof(expected) - 1 - supplied_length);
            if (count > 0) supplied_length += static_cast<size_t>(count);
            else if (count < 0 && errno == EINTR) continue;
            else _exit(128);
        }
        bool accepted = supplied_length == sizeof(expected) - 1 &&
            memcmp(supplied, expected, sizeof(expected) - 1) == 0;
        char auth_result = accepted ? 'P' : 'F';
        if (!write_all(result_pipe[1], &auth_result, 1)) _exit(129);
        if (accepted) {
            const char ready[] = "__RELIEFAPK_WORKER_READY__\n";
            if (!write_all(STDERR_FILENO, ready, sizeof(ready) - 1)) _exit(130);
        }
        sleep_ms(120);
        char terminal_result = pty_helper_saw_hup ? 'H' : 'A';
        if (!write_all(result_pipe[1], &terminal_result, 1)) _exit(131);
        _exit(accepted && !pty_helper_saw_hup ? 0 : 1);
    }

    close(errors_pipe[1]);
    close(result_pipe[1]);
    close(echo_toggle_pipe[0]);
    ra::State state;
    ra::initialize_state(&state);
    state.active = 1;
    state.wrapper = 1;
    state.auth_is_su = 1;
    state.pty_master = master;
    state.pty_slave = slave;

    const char prompt[] = "Password:";
    char received_prompt[sizeof(prompt) - 1];
    bool prompt_received = read_exact_with_timeout(errors_pipe[0], received_prompt,
        sizeof(received_prompt), 2000);
    check(prompt_received && memcmp(received_prompt, prompt, sizeof(received_prompt)) == 0,
        "PTY helper writes its password prompt to stderr");
    bool echo_off = false;
    struct termios attributes;
    if (tcgetattr(slave, &attributes) == 0) echo_off = !(attributes.c_lflag & ECHO);
    check(!echo_off, "stderr prompt can arrive before the helper disables PTY ECHO");

    bool password_sent = false;
    bool ready_marker_received = false;
    if (prompt_received) {
        ra::parse_stderr(&state, received_prompt, sizeof(received_prompt));
        check(!state.auth_prompt,
            "stderr prompt received with PTY ECHO enabled cannot request a password yet");
        check(state.errors_length == 0,
            "authentication prompt text is stripped from operation stderr");
        check(!ra::prepare_password(&state, "pty-test-secret"),
            "password cannot be prepared while terminal ECHO is enabled");
        const char pending_secret[] = "pending-not-echoed\n";
        memcpy(state.password, pending_secret, sizeof(pending_secret) - 1);
        state.password_length = sizeof(pending_secret) - 1;
        ra::send_pending_password(&state);
        int echoed_bytes = -1;
        bool echo_count_read = ioctl(master, FIONREAD, &echoed_bytes) == 0;
        check(echo_count_read && echoed_bytes == 0 &&
            state.password_length == sizeof(pending_secret) - 1,
            "pending password stays buffered and is not written while PTY ECHO is enabled");
        (void)tcflush(slave, TCIOFLUSH);
        ra::wipe_password(&state);
        const char enable_auth = 'E';
        check(write_all(echo_toggle_pipe[1], &enable_auth, 1),
            "allow helper to disable ECHO after emitting its stderr prompt");
        char echo_off_marker = 0;
        bool echo_off_marker_received = read_exact_with_timeout(result_pipe[0],
            &echo_off_marker, 1, 2000);
        check(echo_off_marker_received && echo_off_marker == 'E',
            "helper reports its real PTY echo transition");
        echo_off = tcgetattr(slave, &attributes) == 0 && !(attributes.c_lflag & ECHO);
        check(echo_off, "helper PTY has ECHO disabled before authentication is requested");
        ra::refresh_su_auth_prompt(&state);
        check(state.auth_prompt,
            "deferred prompt is exposed after a step observes PTY ECHO disabled");
        bool password_prepared = ra::prepare_password(&state, "pty-test-secret");
        check(password_prepared,
            "deferred stderr prompt becomes actionable once PTY ECHO is disabled");
        if (password_prepared) {
            ra::send_pending_password(&state);
            password_sent = state.password_length == 0;
        }
        check(password_sent, "password is delivered through the private controlling PTY");
    }

    if (password_sent) {
        char auth_result = 0;
        bool helper_accepted = read_exact_with_timeout(result_pipe[0], &auth_result, 1, 2000);
        check(helper_accepted && auth_result == 'P',
            "helper receives only the entered password on PTY stdin");
        const char ready[] = "__RELIEFAPK_WORKER_READY__\n";
        char received_ready[sizeof(ready) - 1];
        ready_marker_received = read_exact_with_timeout(errors_pipe[0], received_ready,
            sizeof(received_ready), 2000);
        if (ready_marker_received)
            ra::parse_stderr(&state, received_ready, sizeof(received_ready));
        check(ready_marker_received && state.auth_ready,
            "worker-ready marker is still recognized on stderr after su authentication");
        check(state.pty_master == master && fcntl(master, F_GETFD) >= 0,
            "stderr authentication path keeps its controlling PTY master open");
    }

    char terminal_result = 0;
    bool helper_survived = password_sent && ready_marker_received &&
        read_exact_with_timeout(result_pipe[0], &terminal_result, 1, 2000);
    check(helper_survived && terminal_result == 'A',
        "authenticated helper stays alive after the stderr ready marker");
    if (!helper_survived) kill(helper, SIGKILL);
    if (state.pty_master >= 0) ra::close_fd(state.pty_master);
    if (state.pty_slave >= 0) ra::close_fd(state.pty_slave);
    ra::wipe_password(&state);
    free(state.output_data);
    free(state.errors_data);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(helper, &status, 0); } while (waited < 0 && errno == EINTR);
    check(waited == helper && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "stderr/PTY authentication helper exits cleanly");
    close(errors_pipe[0]);
    close(result_pipe[0]);
    close(echo_toggle_pipe[1]);
}

void test_password_buffer_rejects_line_breaks()
{
    ra::State lf_state;
    ra::initialize_state(&lf_state);
    lf_state.active = 1;
    lf_state.auth_prompt = 1;
    check(!ra::prepare_password(&lf_state, "first\nsecond"),
        "reject LF before placing credentials in the password buffer");
    check(lf_state.password_length == 0 && lf_state.auth_prompt,
        "rejected LF leaves the password request pending and buffer empty");

    ra::State cr_state;
    ra::initialize_state(&cr_state);
    cr_state.active = 1;
    cr_state.auth_prompt = 1;
    check(!ra::prepare_password(&cr_state, "first\rsecond"),
        "reject CR before placing credentials in the password buffer");
    check(cr_state.password_length == 0 && cr_state.auth_prompt,
        "rejected CR leaves the password request pending and buffer empty");

    ra::State valid_state;
    ra::initialize_state(&valid_state);
    valid_state.active = 1;
    valid_state.auth_prompt = 1;
    check(ra::prepare_password(&valid_state, "single-line-secret"),
        "accept a single-line password");
    const char expected[] = "single-line-secret\n";
    check(valid_state.password_length == sizeof(expected) - 1 &&
        memcmp(valid_state.password, expected, sizeof(expected) - 1) == 0,
        "accepted password receives exactly one terminal newline");
    ra::wipe_password(&valid_state);
}

void test_exec_failure()
{
    ra::Process process;
    const char *argv[] = {"/definitely/not/a/reliefapk-command", nullptr};
    check(process.start(argv), "exec failure is reported asynchronously");
    check(finish(process), "failed exec is reaped");
    check(process.exec_error() == ENOENT, "exec errno survives the close-on-exec pipe");
    check(process.exit_code() == 127, "failed exec receives conventional status 127");
}

void test_output_caps_and_drain()
{
    ra::Process process;
    const char *argv[] = {self_path, "--child-flood", nullptr};
    check(process.start(argv), "start output flood");
    check(finish(process, 65536, 30000), "flood is drained after capture reaches its caps");
    check(process.output_size() == 64u * 1024u * 1024u, "stdout capture is capped at 64 MiB");
    check(process.errors_size() == 1024u * 1024u, "stderr capture is capped at 1 MiB");
    check(process.truncated(), "discarded output is reported as truncated");
    check(process.exit_code() == 0, "flood child can exit after excess output is discarded");
}

void test_dual_stream_fairness()
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char ready_path[4096];
    char release_path[4096];
    snprintf(ready_path, sizeof(ready_path), "%s/reliefapk-dual-ready-%ld", tmp,
        static_cast<long>(getpid()));
    snprintf(release_path, sizeof(release_path), "%s/reliefapk-dual-release-%ld", tmp,
        static_cast<long>(getpid()));
    unlink(ready_path);
    unlink(release_path);

    ra::Process process;
    const char *argv[] = {self_path, "--child-dual-flood", ready_path, release_path, nullptr};
    bool started = process.start(argv);
    check(started, "start simultaneous stdout and stderr writers");
    bool both_ready = false;
    double ready_deadline = now_seconds() + 3.0;
    while (started && !both_ready && now_seconds() < ready_deadline) {
        struct stat ready = {};
        both_ready = stat(ready_path, &ready) == 0 && ready.st_size >= 2;
        if (!both_ready) sleep_ms(1);
    }
    check(both_ready, "both writers buffer a 4 KiB chunk before fairness polling");
    if (both_ready) {
        process.step(2 * DualStreamPrebuffer);
        check(process.output_size() + process.errors_size() <= 2 * DualStreamPrebuffer,
            "one step shares its byte budget across both ready output streams");
        check(process.output_size() == DualStreamPrebuffer &&
            process.errors_size() == DualStreamPrebuffer,
            "a busy stdout stream does not starve ready stderr");
    }

    int release_fd = open(release_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(release_fd >= 0, "release both producers after the deterministic fairness poll");
    if (release_fd >= 0) close(release_fd);
    else if (started) process.cancel();
    check(finish(process), "both blocked writers complete when both pipes are drained");
    check(process.output_size() == 256u * 1024u && process.errors_size() == 256u * 1024u,
        "simultaneous stream output is fully retained");
    unlink(ready_path);
    unlink(release_path);
}

void test_cancel_and_start_guards()
{
    ra::Process process;
    const char *argv[] = {self_path, "--child-wait", nullptr};
    check(process.start(argv), "start cancelable process");
    check(!process.start(argv), "second start does not replace a live child");
    sleep_ms(20);
    check(process.cancel(), "cancel sends a signal to the operation process group");
    check(finish(process), "canceled process is reaped");
    check(process.exit_code() == 128 + SIGTERM, "TERM cancellation reports signal status");

    ra::Process protected_process;
    check(protected_process.start(argv, false, ra::AuthMode::Sudo, false),
        "start a noncancelable process");
    check(!protected_process.cancel(), "noncancelable operation rejects cancel");
}

void test_orphan_pipe_holder_is_cleaned(const char *mode, const char *suffix)
{
    char path[4096];
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(path, sizeof(path), "%s/reliefapk-orphan-%s-%ld", tmp, suffix,
        static_cast<long>(getpid()));
    unlink(path);
    ra::Process process;
    const char *argv[] = {self_path, mode, path, nullptr};
    const double before = now_seconds();
    check(process.start(argv), "start helper which leaves a pipe-holding descendant");
    check(finish(process), "leader exit does not leave polling hung on descendant pipes");
    check(now_seconds() - before < 2.0, "descendant cleanup is bounded");
    sleep_ms(60);
    struct stat first = {};
    bool has_marker = stat(path, &first) == 0;
    check(has_marker, "descendant wrote a runtime marker before cleanup");
    sleep_ms(120);
    struct stat later = {};
    bool still_stable = stat(path, &later) == 0 && has_marker && later.st_size == first.st_size;
    check(still_stable,
        strcmp(suffix, "open") == 0 ? "pipe-holding descendant is terminated after leader exit" :
            "closed-stream descendant is terminated after leader exit");
    unlink(path);
}

void test_repeated_start_and_root_direct_path()
{
    ra::Process process;
    const char *argv[] = {self_path, "--child-streams", nullptr};
    check(process.start(argv), "first run starts");
    check(finish(process), "first run completes");
    check(process.start(argv), "completed process can be safely replaced");
    check(finish(process), "replacement run completes");
    check(process.exit_code() == 7 && process.output_size() == 6,
        "replacement run resets completed output and status");

    if (geteuid() == 0) {
        const char *root_argv[] = {self_path, "--child-streams", nullptr};
        ra::Process root_process;
        check(root_process.start(root_argv, true, ra::AuthMode::Sudo),
            "root privileged mode starts original argv directly");
        check(finish(root_process), "root direct command completes");
        check(root_process.exit_code() == 7, "root direct command preserves status");
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[1], "--child-", 8) == 0) return child_main(argc, argv);
    char resolved[4096];
    if (!realpath(argv[0], resolved)) {
        perror("realpath");
        return 2;
    }
    self_path = strdup(resolved);
    if (!self_path) return 2;

    test_streams_exit_and_budget();
    test_slow_process_is_pollable();
    test_child_locale_is_normalized_without_changing_parent();
    test_authentication_ready_keeps_su_controlling_pty();
    test_su_stderr_prompt_uses_real_pty_echo_state();
    test_password_buffer_rejects_line_breaks();
    test_exec_failure();
    test_output_caps_and_drain();
    test_dual_stream_fairness();
    test_cancel_and_start_guards();
    test_orphan_pipe_holder_is_cleaned("--child-orphan", "open");
    test_orphan_pipe_holder_is_cleaned("--child-orphan-closed", "closed");
    test_repeated_start_and_root_direct_path();

    free(const_cast<char *>(self_path));
    if (failures) {
        fprintf(stderr, "ReliefAPK process tests: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("ReliefAPK process tests: %d checks passed\n", checks);
    return 0;
}
