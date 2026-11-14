#include "autostart.h"
#include <gio/gdesktopappinfo.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace autostart {
namespace {
volatile sig_atomic_t stopping;
void stop(int) { stopping = 1; }
void remember(GDesktopAppInfo *, GPid pid, gpointer data)
{
    auto *children = static_cast<Array<pid_t> *>(data);
    if (!children->push(pid)) kill(pid, SIGTERM);
}
void reap(Array<pid_t> &children)
{
    for (size_t n = 0; n < children.size();) {
        pid_t result = waitpid(children[n], nullptr, WNOHANG);
        if (result == children[n] || (result < 0 && errno == ECHILD)) children.erase(n);
        else ++n;
    }
}

int session_lock()
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    Text base;
    if (runtime && runtime[0] == '/' && !lstat(runtime, &st) && S_ISDIR(st.st_mode) &&
        st.st_uid == getuid() && !(st.st_mode & 0077)) base.set(runtime);
    else {
        const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        char *path = state && state[0] == '/' ? g_build_filename(state, "reliefos", "autostart", nullptr)
                                            : g_build_filename(home, ".local", "state", "reliefos", "autostart", nullptr);
        base.set(path);
        g_free(path);
        if (g_mkdir_with_parents(base.c_str(), 0700)) return -1;
        if (lstat(base.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) return -1;
    }
    const char *display = getenv("DISPLAY");
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, display ? display : "", -1);
    char *path = g_strdup_printf("%s/reliefos-autostart-%s.lock", base.c_str(), digest);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    g_free(digest);
    g_free(path);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
        close(fd);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int result = errno == EWOULDBLOCK ? -2 : -1;
        close(fd);
        return result;
    }
    return fd;
}
} // namespace

int run_session()
{
    Paths paths;
    Text error;
    if (!paths.initialize(error)) { fprintf(stderr, "autostart: %s\n", error.c_str()); return 1; }
    int lock = session_lock();
    if (lock == -2) return 0;
    if (lock < 0) { fputs("autostart: cannot lock the session\n", stderr); return 1; }
    stopping = 0;
    struct sigaction action = {};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    // GIO calls this private adapter with an already expanded argv for Terminal=true.
    // Its location is confined to this runner and its children.
    const char *path = getenv("PATH");
    char *terminal_path = g_strdup_printf("/usr/libexec/reliefos/autostart:%s", path ? path : "/usr/bin:/bin");
    setenv("PATH", terminal_path, 1);
    g_free(terminal_path);
    Snapshot snapshot;
    Array<pid_t> children;
    if (!scan(paths, snapshot)) fprintf(stderr, "autostart: %s\n", snapshot.error.c_str());
    else for (const auto &entry : snapshot.entries) {
        if (stopping) break;
        char *safe = g_strescape(entry.id.c_str(), nullptr);
        if (entry.state != State::Ready) {
            const char *states[] = {"enabled", "disabled", "other desktop", "program unavailable", "invalid entry"};
            fprintf(stderr, "autostart: %s skipped (%s)\n", safe, states[static_cast<unsigned>(entry.state)]);
            g_free(safe);
            continue;
        }
        GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(entry.source.c_str());
        if (!app) { fprintf(stderr, "autostart: %s changed or became unavailable\n", safe); g_free(safe); continue; }
        // Recheck conditions on the object used for launch if the file changed.
        bool allowed = !g_desktop_app_info_get_is_hidden(app);
        gchar **desktops = g_strsplit(paths.desktop.c_str(), ":", -1);
        bool show = false;
        for (size_t n = 0; desktops[n]; ++n) show |= g_desktop_app_info_get_show_in(app, desktops[n]);
        g_strfreev(desktops);
        GError *failure = nullptr;
        if (allowed && show && !g_desktop_app_info_launch_uris_as_manager(app, nullptr, nullptr,
                GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
                nullptr, nullptr, remember, &children, &failure)) {
            // Entry IDs and numeric errors suffice without logging command arguments.
            fprintf(stderr, "autostart: %s failed (%u/%d)\n", safe,
                    failure ? failure->domain : 0, failure ? failure->code : 0);
        } else fprintf(stderr, "autostart: %s %s\n", safe, allowed && show ? "started" : "skipped (conditions changed)");
        g_free(safe);
        if (failure) g_error_free(failure);
        g_object_unref(app);
        reap(children);
    }
    snapshot.entries.clear();
    while (!stopping) {
        reap(children);
        // Dispatch GIO's D-Bus activation completions without a second UI loop.
        while (!stopping && g_main_context_iteration(nullptr, false)) {}
        usleep(100000);
    }
    reap(children);
    for (pid_t pid : children) kill(pid, SIGTERM);
    for (unsigned n = 0; n < 20 && !children.empty(); ++n) { reap(children); usleep(100000); }
    for (pid_t pid : children) kill(pid, SIGKILL);
    for (pid_t pid : children) while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    close(lock);
    return snapshot.error.empty() ? 0 : 1;
}
} // namespace autostart
