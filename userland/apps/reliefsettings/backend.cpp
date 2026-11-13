#include "backend.h"
extern "C" {
#include "../../runtime/src/locale_conf.h"
}
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace rs {
namespace {
void reply(int fd, char status, const Text &error = Text{}) {
    Reply message;
    message.status = status;
    snprintf(message.error, sizeof(message.error), "%s", error.c_str());
    const char *bytes = reinterpret_cast<const char *>(&message);
    size_t sent = 0;
    while (sent < sizeof(message)) {
        ssize_t n = send(fd, bytes + sent, sizeof(message) - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        sent += n;
    }
}
bool save(const SaveRequest &request, Text &error, bool &published) {
    Store store(request.directory);
    return store.save(request.name, *request.expected, *request.bytes, error, &published);
}
bool keyboard(const char *layout, const char *variant, Text &error) {
    const char *argv[] = {"/usr/bin/setxkbmap", "-layout", layout, "-variant", variant, nullptr};
    Text output;
    return command(argv, output, error);
}
bool keyboard_value(const char *query, const char *key, Text &value) {
    size_t length = strlen(key);
    for (const char *p = query; *p;) {
        const char *end = strchr(p, '\n');
        size_t n = end ? static_cast<size_t>(end - p) : strlen(p);
        if (n > length && !memcmp(p, key, length) && p[length] == ':') {
            const char *first = p + length + 1;
            while (*first == ' ' || *first == '\t')
                ++first;
            return value.set(first, p + n - first);
        }
        p += n + (end ? 1 : 0);
    }
    value.clear();
    return true;
}
} // namespace
double seconds() {
    struct timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}
bool command(const char *const argv[], Text &output, Text &error) {
    ra::Process process;
    if (!process.start(argv)) {
        error.set(strerror(errno));
        return false;
    }
    double deadline = seconds() + 10;
    bool timed_out = false;
    while (process.running()) {
        process.step(65536);
        if (!timed_out && seconds() >= deadline) {
            process.cancel();
            timed_out = true;
        }
        if (process.running())
            poll(nullptr, 0, 10);
    }
    if (timed_out) {
        error.set("The command timed out.");
        return false;
    }
    if (process.exit_code() || process.exec_error() || process.truncated()) {
        error.set(*process.errors() ? process.errors() : "The command failed.");
        return false;
    }
    if (!output.set(process.output(), process.output_size())) {
        error.set("Out of memory.");
        return false;
    }
    return true;
}
bool apply_mode(const DisplaySetting &mode, Text &error) {
    const char *argv[] = {"/usr/bin/xrandr", "--output", mode.output.c_str(), "--mode",
                          mode.mode.c_str(), "--rate",   mode.rate.c_str(),   nullptr};
    Text output;
    return command(argv, output, error);
}
Job::~Job() {
    close();
}
bool Job::start(void (*work)(int, void *), void *context, int display_fd) {
    if (active())
        return false;
    if (pid_ > 0) {
        if (waitpid(pid_, nullptr, WNOHANG) == 0)
            return false;
        pid_ = -1;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
        return false;
    pid_ = fork();
    if (!pid_) {
        ::close(pair[0]);
        if (display_fd >= 0)
            ::close(display_fd);
        signal(SIGPIPE, SIG_IGN);
        work(pair[1], context);
        ::close(pair[1]);
        _exit(0);
    }
    ::close(pair[1]);
    if (pid_ < 0) {
        ::close(pair[0]);
        return false;
    }
    int flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(pair[0]);
        return false;
    }
    socket_ = pair[0];
    received_ = 0;
    return true;
}
bool Job::poll(Reply &message) {
    if (!active())
        return false;
    for (;;) {
        ssize_t n = read(socket_, reinterpret_cast<char *>(&pending_) + received_,
                         sizeof(pending_) - received_);
        if (n > 0) {
            received_ += n;
            if (received_ == sizeof(pending_)) {
                message = pending_;
                received_ = 0;
                message.error[sizeof(message.error) - 1] = 0;
                if (message.status != 'R')
                    close();
                return true;
            }
        } else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return false;
        else {
            message = Reply{};
            message.status = 'E';
            snprintf(message.error, sizeof(message.error), "%s",
                     "The settings worker exited unexpectedly.");
            close();
            return true;
        }
    }
}
bool Job::decide(bool keep) {
    return socket_ >= 0 && send(socket_, keep ? "K" : "R", 1, MSG_NOSIGNAL) == 1;
}
void Job::close() {
    if (socket_ >= 0)
        ::close(socket_);
    socket_ = -1;
    if (pid_ > 0 && waitpid(pid_, nullptr, WNOHANG) != 0)
        pid_ = -1;
}
void save_worker(int channel, void *context) {
    Text error;
    bool published = false;
    bool ok = save(*static_cast<SaveRequest *>(context), error, published);
    reply(channel, ok ? 'S' : (published ? 'W' : 'E'), error);
}
void keyboard_worker(int channel, void *context) {
    auto &request = *static_cast<KeyboardRequest *>(context);
    Store store(request.save.directory);
    Text error, query, old_layout, old_variant;
    int lock = store.lock_desktop(error);
    if (lock < 0) {
        reply(channel, 'E', error);
        return;
    }
    const char *argv[] = {"/usr/bin/setxkbmap", "-query", nullptr};
    bool ok = command(argv, query, error) && keyboard_value(query.c_str(), "layout", old_layout) &&
              keyboard_value(query.c_str(), "variant", old_variant) && !old_layout.empty();
    if (!ok) {
        if (error.empty())
            error.set("Cannot read the current keyboard layout.");
        close(lock);
        reply(channel, 'E', error);
        return;
    }
    ok = keyboard(request.layout, request.variant, error);
    bool published = false;
    if (ok)
        ok = save(request.save, error, published);
    if (!ok && !published) {
        Text rollback;
        if (!keyboard(old_layout.c_str(), old_variant.c_str(), rollback)) {
            append(error, "\nKeyboard restore failed\n");
            append(error, rollback.c_str());
        }
    }
    close(lock);
    reply(channel, ok ? 'S' : (published ? 'W' : 'E'), error);
}
void display_worker(int channel, void *context) {
    auto &request = *static_cast<DisplayRequest *>(context);
    Store store(request.save.directory);
    Text error, query;
    int lock = store.lock_desktop(error);
    if (lock < 0) {
        reply(channel, 'E', error);
        return;
    }
    Array<Output> outputs;
    DisplaySetting before;
    const char *argv[] = {"/usr/bin/xrandr", "--query", nullptr};
    bool ready = command(argv, query, error) && parse_outputs(query.c_str(), outputs, error);
    bool offered = false;
    for (size_t i = 0; ready && i < outputs.size(); ++i) {
        const auto &output = outputs[i];
        if (!(output.name == request.after->output) || output.current < 0)
            continue;
        before = {output.name, output.modes[output.current].name,
                  output.modes[output.current].rate};
        for (size_t m = 0; m < output.modes.size(); ++m)
            offered |= output.modes[m].name == request.after->mode &&
                       rate_equal(output.modes[m].rate, request.after->rate);
    }
    if (!ready || !offered) {
        if (error.empty())
            error.set("The display mode changed. Reload and try again.");
        close(lock);
        reply(channel, 'E', error);
        return;
    }
    bool applied = apply_mode(*request.after, error);
    bool keep = false;
    if (applied) {
        reply(channel, 'R');
        double deadline = seconds() + 15;
        while (seconds() < deadline) {
            struct pollfd fd{channel, POLLIN | POLLHUP, 0};
            int wait = static_cast<int>((deadline - seconds()) * 1000);
            int result = poll(&fd, 1, wait > 0 ? wait : 0);
            if (result < 0 && errno == EINTR)
                continue;
            if (result > 0) {
                char byte;
                keep = read(channel, &byte, 1) == 1 && byte == 'K';
            }
            break;
        }
    }
    bool published = false;
    bool saved = keep && save(request.save, error, published);
    if (saved || published) {
        close(lock);
        reply(channel, saved ? 'S' : 'W', error);
        return;
    }
    Text rollback;
    bool restored = apply_mode(before, rollback);
    if (!restored) {
        append(error, "\nDisplay restore failed\n");
        append(error, rollback.c_str());
    }
    close(lock);
    reply(channel, applied && !keep && restored ? 'B' : 'E', error);
}
bool load_zones(Array<Text> &zones, Text &error) {
    Text contents;
    if (!read_text("/usr/share/zoneinfo/zone.tab", contents, error))
        return false;
    Array<Text> next;
    Text utc;
    if (!utc.set("Etc/UTC") || !next.push(utc)) {
        error.set("Out of memory.");
        return false;
    }
    char *copy = strdup(contents.c_str());
    if (!copy) {
        error.set("Out of memory.");
        return false;
    }
    char *lines;
    bool ok = true;
    for (char *line = strtok_r(copy, "\n", &lines); line; line = strtok_r(nullptr, "\n", &lines)) {
        if (*line == '#')
            continue;
        char *fields;
        strtok_r(line, "\t", &fields);
        strtok_r(nullptr, "\t", &fields);
        char *zone = strtok_r(nullptr, "\t", &fields);
        if (!valid_zone(zone))
            continue;
        Text name;
        if (!name.set(zone) || !next.push(name)) {
            ok = false;
            break;
        }
    }
    free(copy);
    if (!ok) {
        error.set("Out of memory.");
        return false;
    }
    zones = static_cast<Array<Text> &&>(next);
    return true;
}
Text current_timezone() {
    Text result;
    char path[4096];
    ssize_t n = readlink("/etc/localtime", path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = 0;
        const char *zone = strstr(path, "zoneinfo/");
        if (zone && valid_zone(zone + 9))
            result.set(zone + 9);
    }
    return result;
}
int session_environment() {
    Text directory, error;
    if (!config_directory(directory, error))
        return 1;
    Store store(directory.c_str());
    Snapshot snapshot;
    if (!store.read("locale.conf", snapshot, error))
        return 1;
    struct reliefos_locale_setting settings[RELIEFOS_LOCALE_MAX];
    int n = reliefos_locale_parse(snapshot.bytes.c_str(), snapshot.bytes.size(), settings,
                                  RELIEFOS_LOCALE_MAX);
    for (int i = 0; i < n; ++i)
        printf("%s=%s\n", settings[i].name, settings[i].value);
    return 0;
}
int apply_session() {
    Text directory, error;
    if (!config_directory(directory, error))
        return 1;
    Store store(directory.c_str());
    Snapshot snapshot;
    Desktop desktop;
    if (!store.read("desktop.conf", snapshot, error) ||
        !parse_desktop(snapshot.bytes.c_str(), desktop, error)) {
        fprintf(stderr, "[reliefsettings] %s\n", error.c_str());
        return 1;
    }
    int lock = -1;
    if (!desktop.layout.empty() || !desktop.displays.empty()) {
        lock = store.lock_desktop(error);
        if (lock < 0) {
            fprintf(stderr, "[reliefsettings] %s\n", error.c_str());
            return 1;
        }
    }
    bool ok = true;
    if (!desktop.layout.empty())
        ok = keyboard(desktop.layout.c_str(), desktop.variant.c_str(), error);
    if (!ok)
        fprintf(stderr, "[reliefsettings] keyboard: %s\n", error.c_str());
    if (!desktop.displays.empty()) {
        const char *argv[] = {"/usr/bin/xrandr", "--query", nullptr};
        Text query;
        Array<Output> outputs;
        if (!command(argv, query, error) || !parse_outputs(query.c_str(), outputs, error)) {
            close(lock);
            return 1;
        }
        for (size_t i = 0; i < desktop.displays.size(); ++i) {
            const auto &mode = desktop.displays[i];
            bool offered = false;
            for (size_t o = 0; o < outputs.size(); ++o) {
                if (!(outputs[o].name == mode.output) || outputs[o].current < 0)
                    continue;
                for (size_t m = 0; m < outputs[o].modes.size(); ++m)
                    offered |= outputs[o].modes[m].name == mode.mode &&
                               rate_equal(outputs[o].modes[m].rate, mode.rate);
            }
            if (offered && !apply_mode(mode, error)) {
                ok = false;
                fprintf(stderr, "[reliefsettings] display: %s\n", error.c_str());
            }
        }
    }
    if (lock >= 0)
        close(lock);
    return ok ? 0 : 1;
}
} // namespace rs
