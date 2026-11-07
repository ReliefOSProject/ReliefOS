#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <reliefos/api.h>
#include <reliefos/sudo.h>
#include <reliefos/auth.h>
#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <reliefos/http.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/launch.h>
#include <reliefos/xdg.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <string.h>
#include <reliefos/layout.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define T(s) gettext(s)

#define WIZARD_W 480U
#define WIZARD_H 360U
#define DOWNLOAD_PATH_MAX RELIEFOS_FS_PATH_LEN
#define DOWNLOAD_STATUS_PATH_MAX 96U
#define DOWNLOAD_UPDATE_BYTES (64U * 1024U)
#define DOWNLOAD_UPDATE_MS 100U
#define INSTALL_UPDATE_BYTES (64U * 1024U)
#define INSTALL_UPDATE_MS 100U
#define API_INSTALL_LOG_PATH "/var/log/apiapp-install.log"
#define TASK_STATE_EXITED 3U
#define APIAPP_PATH RELIEFOS_LAYOUT_RELIEFOS_APPS "/apiapp/apiapp.elf"

static uint32_t wizard_pixels[WIZARD_W * WIZARD_H];

struct install_state {
    char api_path[RELIEFOS_API_PATH_MAX];
    char install_path[RELIEFOS_API_PATH_MAX];
    uint32_t create_shortcut;
    struct reliefos_api_info info;
    int step;
};

struct download_state {
    int window_id;
    int worker_pid;
    char output_path[DOWNLOAD_PATH_MAX];
    char status_path[DOWNLOAD_STATUS_PATH_MAX];
    char status[128];
    uint32_t received;
    uint32_t total;
    uint8_t cancelled;
};

struct download_worker_state {
    char status_path[DOWNLOAD_STATUS_PATH_MAX];
    uint32_t last_received;
    unsigned long last_update_ms;
};

struct install_worker_state {
    int progress_fd;
    uint32_t last_processed;
    uint32_t total;
    unsigned long last_update_ms;
};

struct install_progress {
    uint32_t processed;
    uint32_t total;
};

static void copy_text(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || capacity == 0) {
        return;
    }
    while (src && src[i] && i + 1U < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void append_text(char *dst, uint32_t *pos, uint32_t capacity,
                        const char *src)
{
    while (src && *src && *pos + 1U < capacity) {
        dst[(*pos)++] = *src++;
    }
    dst[*pos] = 0;
}

static void append_u32(char *dst, uint32_t *pos, uint32_t capacity,
                       uint32_t value)
{
    char digits[12];
    uint32_t count = 0;
    if (value == 0) {
        if (*pos + 1U < capacity) {
            dst[(*pos)++] = '0';
            dst[*pos] = 0;
        }
        return;
    }
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count && *pos + 1U < capacity) {
        dst[(*pos)++] = digits[--count];
    }
    dst[*pos] = 0;
}

static void download_path_for_user(char *dst, uint32_t capacity)
{
    if (reliefos_xdg_user_dir("DOWNLOAD",dst,capacity)<0) copy_text(dst,capacity,"/tmp");
}

static void build_download_path(char *dst, uint32_t capacity)
{
    char directory[RELIEFOS_FS_PATH_LEN];
    uint32_t pos = 0;
    download_path_for_user(directory, sizeof(directory));
    (void)mkdir(directory, 0700);
    append_text(dst, &pos, capacity, directory);
    append_text(dst, &pos, capacity, "/app-");
    append_u32(dst, &pos, capacity, (uint32_t)getpid());
    append_text(dst, &pos, capacity, ".api");
}

static void build_download_status_path(char *dst, uint32_t capacity)
{
    uint32_t pos = 0;
    (void)mkdir("/tmp", 01777);
    dst[0] = 0;
    append_text(dst, &pos, capacity, "/tmp/api_download_");
    append_u32(dst, &pos, capacity, (uint32_t)getpid());
    append_text(dst, &pos, capacity, ".status");
}

static void install_log(const char *message)
{
    int fd;
    long wrote;
    uint32_t len;
    if (!message) {
        return;
    }
    printf("[apiapp] %s\n", message);
    (void)mkdir("/var", 0755);
    (void)mkdir("/var/log", 0755);
    fd = open(API_INSTALL_LOG_PATH,
              RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_APPEND, 0666);
    if (fd < 0) {
        return;
    }
    len = (uint32_t)strlen(message);
    wrote = write(fd, message, len);
    if (wrote == (long)len) {
        (void)write(fd, "\n", 1);
    }
    close(fd);
}

static void install_log_path(const char *label, const char *path)
{
    char line[RELIEFOS_FS_PATH_LEN + 48U];
    uint32_t pos = 0;
    line[0] = 0;
    append_text(line, &pos, sizeof(line), label);
    append_text(line, &pos, sizeof(line), path ? path : "(null)");
    install_log(line);
}

static void install_log_result(const char *label, int result)
{
    char line[96];
    uint32_t pos = 0;
    line[0] = 0;
    append_text(line, &pos, sizeof(line), label);
    append_u32(line, &pos, sizeof(line),
               result < 0 ? (uint32_t)(-(int64_t)result) : (uint32_t)result);
    install_log(line);
}

static void install_log_progress(uint32_t processed, uint32_t total)
{
    char line[96];
    uint32_t pos = 0;
    line[0] = 0;
    append_text(line, &pos, sizeof(line), "final progress: ");
    append_u32(line, &pos, sizeof(line), processed);
    append_text(line, &pos, sizeof(line), "/");
    append_u32(line, &pos, sizeof(line), total);
    install_log(line);
}

static void build_download_part_path(char *dst, uint32_t capacity,
                                     const char *output_path)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_text(dst, &pos, capacity, output_path);
    append_text(dst, &pos, capacity, ".part");
}

static int write_download_status(const char *path, char state,
                                 uint32_t received, uint32_t total)
{
    char text[64];
    uint32_t pos = 0;
    int fd;
    long wrote;
    if (!path || !path[0]) {
        return -1;
    }
    text[pos++] = state;
    text[pos++] = ' ';
    text[pos] = 0;
    append_u32(text, &pos, sizeof(text), received);
    text[pos++] = ' ';
    text[pos] = 0;
    append_u32(text, &pos, sizeof(text), total);
    text[pos++] = '\n';
    text[pos] = 0;
    fd = open(path, RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0666);
    if (fd < 0) {
        return -1;
    }
    wrote = write(fd, text, pos);
    close(fd);
    return wrote == (long)pos ? 0 : -1;
}

static uint32_t parse_u32(const char **text)
{
    uint32_t value = 0;
    while (text && *text && **text >= '0' && **text <= '9') {
        value = value * 10U + (uint32_t)(**text - '0');
        ++(*text);
    }
    return value;
}

static int read_download_status(const char *path, char *state,
                                uint32_t *received, uint32_t *total)
{
    char text[64];
    const char *cursor;
    int fd;
    long got;
    if (!path || !state || !received || !total) {
        return -1;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    got = read(fd, text, sizeof(text) - 1U);
    close(fd);
    if (got < 3) {
        return -1;
    }
    text[got] = 0;
    if (text[1] != ' ') {
        return -1;
    }
    *state = text[0];
    cursor = text + 2;
    *received = parse_u32(&cursor);
    if (*cursor != ' ') {
        return -1;
    }
    ++cursor;
    *total = parse_u32(&cursor);
    return 0;
}

static int download_worker_exited(int pid)
{
    struct reliefos_task_info tasks[RELIEFOS_TASK_MAX];
    uint64_t tick;
    int count = reliefos_task_snapshot(tasks, RELIEFOS_TASK_MAX, &tick);
    (void)tick;
    if (count < 0) {
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        if ((int)tasks[i].pid == pid) {
            return tasks[i].state == TASK_STATE_EXITED;
        }
    }
    return 1;
}

static void cleanup_download(const struct download_state *state)
{
    char part_path[DOWNLOAD_PATH_MAX];
    if (!state) {
        return;
    }
    build_download_part_path(part_path, sizeof(part_path), state->output_path);
    unlink(part_path);
    unlink(state->status_path);
}

static int hit(int32_t x, int32_t y, uint32_t rx, uint32_t ry,
               uint32_t rw, uint32_t rh)
{
    return x >= (int32_t)rx && y >= (int32_t)ry &&
           x < (int32_t)(rx + rw) && y < (int32_t)(ry + rh);
}

static void draw_download_page(struct reliefos_ui_surface *ui,
                               const struct download_state *state)
{
    uint32_t percent = state->total
                           ? (uint32_t)(((uint64_t)state->received * 100U) /
                                        state->total)
                           : 0U;
    reliefos_ui_rect(ui, 0, 0, WIZARD_W, WIZARD_H, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 24, T("Downloading Application"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 64, state->status, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    if (state->total) {
        reliefos_ui_progress(ui, 32, 100, WIZARD_W - 64U, 24, percent, 100);
    } else {
        reliefos_ui_activity_bar(ui, 32, 107, WIZARD_W - 64U, 10,
                               (uint32_t)(reliefos_uptime_ms() % 1000UL));
    }
    reliefos_ui_text(ui, 32, 144,
                   T("Downloaded"), RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    {
        char detail[96];
        uint32_t pos = 0;
        detail[0] = 0;
        append_u32(detail, &pos, sizeof(detail), state->received);
        append_text(detail, &pos, sizeof(detail), T(" bytes"));
        if (state->total) {
            append_text(detail, &pos, sizeof(detail), T(" of "));
            append_u32(detail, &pos, sizeof(detail), state->total);
        }
        reliefos_ui_text(ui, 32, 168, detail, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    }
    reliefos_ui_button(ui, WIZARD_W - 120U, WIZARD_H - 52U, 88U,
                      RELIEFOS_UI_BUTTON_H, T("Cancel"), 0);
}

static void draw_install_progress_page(struct reliefos_ui_surface *ui,
                                       const struct download_state *state)
{
    uint32_t percent = state->total
                           ? (uint32_t)(((uint64_t)state->received * 100U) /
                                        state->total)
                           : 0U;
    char detail[96];
    uint32_t pos = 0;
    reliefos_ui_rect(ui, 0, 0, WIZARD_W, WIZARD_H, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 24, T("Installing Application"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 64, state->status, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    if (state->total) {
        reliefos_ui_progress(ui, 32, 100, WIZARD_W - 64U, 24, percent, 100);
    } else {
        reliefos_ui_activity_bar(ui, 32, 107, WIZARD_W - 64U, 10,
                               (uint32_t)(reliefos_uptime_ms() % 1000UL));
    }
    detail[0] = 0;
    append_u32(detail, &pos, sizeof(detail), state->received);
    append_text(detail, &pos, sizeof(detail), T(" bytes processed"));
    if (state->total) {
        append_text(detail, &pos, sizeof(detail), T(" of "));
        append_u32(detail, &pos, sizeof(detail), state->total);
    }
    reliefos_ui_text(ui, 32, 144, detail, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 184,
                   T("Please keep this window open until installation completes."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static int download_worker_progress(uint32_t received, uint32_t total,
                                    void *context)
{
    struct download_worker_state *state = (struct download_worker_state *)context;
    unsigned long now;
    if (!state) {
        return -1;
    }
    now = reliefos_uptime_ms();
    if (received && received - state->last_received < DOWNLOAD_UPDATE_BYTES &&
        now - state->last_update_ms < DOWNLOAD_UPDATE_MS) {
        return 0;
    }
    state->last_received = received;
    state->last_update_ms = now;
    return write_download_status(state->status_path, 'R', received, total);
}

static int install_worker_progress(uint32_t processed, uint32_t total,
                                   void *context)
{
    struct install_worker_state *state = (struct install_worker_state *)context;
    unsigned long now;
    if (!state) {
        return -1;
    }
    now = reliefos_uptime_ms();
    if (processed && processed - state->last_processed < INSTALL_UPDATE_BYTES &&
        now - state->last_update_ms < INSTALL_UPDATE_MS) {
        return 0;
    }
    state->last_processed = processed;
    state->total = total;
    state->last_update_ms = now;
    struct install_progress progress = {processed, total};
    size_t sent = 0;
    while (sent < sizeof(progress)) {
        ssize_t n = write(state->progress_fd, (const char *)&progress + sent, sizeof(progress) - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int starts_with_ignore_case(const char *text, const char *prefix)
{
    uint32_t i = 0;
    if (!text || !prefix) {
        return 0;
    }
    while (prefix[i]) {
        char a = text[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static int run_download_worker(const char *url, const char *output_path,
                               const char *status_path)
{
    struct download_worker_state state;
    struct reliefos_http_response response;
    int ret;
    memset(&state, 0, sizeof(state));
    copy_text(state.status_path, sizeof(state.status_path), status_path);
    if (write_download_status(state.status_path, 'R', 0, 0) < 0) {
        return 1;
    }
    ret = reliefos_http_download(url, output_path, RELIEFOS_HTTP_DEFAULT_TIMEOUT_MS,
                               download_worker_progress, &state, &response);
    if (ret == 0 && response.net_status == RELIEFOS_NET_STATUS_OK &&
        response.http_status >= 200U && response.http_status < 300U) {
        (void)write_download_status(state.status_path, 'D', response.body_len,
                                    response.content_length);
        return 0;
    }
    (void)write_download_status(state.status_path, 'F', state.last_received, 0);
    return 1;
}

static int run_install_worker(const char *api_path, const char *install_path,
                              uint32_t create_shortcut)
{
    struct install_worker_state state;
    struct stat output;
    int ret;
    if (geteuid() != 0 || fstat(STDOUT_FILENO, &output) < 0 || !S_ISFIFO(output.st_mode)) return 126;
    memset(&state, 0, sizeof(state));
    state.progress_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (state.progress_fd < 0) return 1;
    /* Keep library diagnostics out of the private progress pipe. */
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) { close(state.progress_fd); return 1; }
    install_log("install worker started");
    install_log_path("api path: ", api_path);
    install_log_path("install path: ", install_path);
    if (install_worker_progress(0, 0, &state) < 0) {
        close(state.progress_fd);
        return 1;
    }
    install_log("calling reliefos_api_install_with_progress");
    ret = reliefos_api_install_with_progress(api_path, install_path,
                                            create_shortcut,
                                            install_worker_progress, &state);
    install_log_progress(state.last_processed, state.total);
    install_log_result("reliefos_api_install_with_progress result: ", ret);
    close(state.progress_fd);
    install_log(ret ? "install worker completed" : "install worker failed");
    return ret ? 0 : 1;
}

static int install_api_with_progress(int window_id, const char *api_path,
                                     const char *install_path,
                                     uint32_t create_shortcut)
{
    struct download_state state;
    struct reliefos_gui_app_event event;
    struct reliefos_ui_surface ui;
    struct install_progress progress;
    size_t received = 0;
    char *argv[6];
    int pair[2];
    uint32_t child;
    int worker_status;
    memset(&state, 0, sizeof(state));
    state.window_id = window_id;
    install_log("installation requested");
    install_log_path("api path: ", api_path);
    install_log_path("install path: ", install_path);
    copy_text(state.status, sizeof(state.status),
              T("Authorizing installation..."));
    reliefos_ui_bind(&ui, wizard_pixels, WIZARD_W, WIZARD_H, WIZARD_W);
    argv[0] = APIAPP_PATH;
    argv[1] = "--install-worker";
    argv[2] = (char *)api_path;
    argv[3] = (char *)install_path;
    argv[4] = create_shortcut ? "1" : "0";
    argv[5] = 0;
    if (pipe2(pair, O_CLOEXEC) < 0) return 0;
    if (fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0 ||
        reliefos_sudo_run_stdout(NULL, argv, pair[1], &child) < 0) {
        int error = errno;
        close(pair[0]); close(pair[1]);
        errno = error;
        install_log("failed to start authorized install worker");
        return 0;
    }
    close(pair[1]);
    for (;;) {
        for (unsigned budget = 0; budget < 32; ++budget) {
            ssize_t n = read(pair[0], (char *)&progress + received, sizeof(progress) - received);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            received += (size_t)n;
            if (received != sizeof(progress)) continue;
            state.received = progress.processed;
            state.total = progress.total;
            received = 0;
            copy_text(state.status, sizeof(state.status), T("Installing..."));
        }
        if (reliefos_sudo_wait(child, &worker_status) == 0) {
            int complete = WIFEXITED(worker_status) && WEXITSTATUS(worker_status) == 0;
            close(pair[0]);
            install_log_result("install worker exit status: ", worker_status);
            install_log(complete ? "installation completed" : "installation failed or authorization cancelled");
            return complete;
        }
        if (errno != EINTR && errno != EAGAIN) {
            close(pair[0]);
            install_log("failed to wait for authorized install worker");
            return 0;
        }
        draw_install_progress_page(&ui, &state);
        reliefos_gui_present_window((uint32_t)window_id, WIZARD_W, WIZARD_H,
                                  WIZARD_W, wizard_pixels);
        event.window_id = (uint32_t)window_id;
        (void)reliefos_gui_wait_app_event(&event, 50U);
    }
}

static int wait_for_download_close(struct download_state *state,
                                   struct reliefos_ui_surface *ui)
{
    struct reliefos_gui_app_event event;
    for (;;) {
        draw_download_page(ui, state);
        reliefos_gui_present_window((uint32_t)state->window_id, WIZARD_W,
                                  WIZARD_H, WIZARD_W, wizard_pixels);
        event.window_id = (uint32_t)state->window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0 &&
            (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE ||
             (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
              (event.buttons & 1U) &&
              hit(event.x, event.y, WIZARD_W - 120U, WIZARD_H - 52U,
                  88U, RELIEFOS_UI_BUTTON_H)))) {
            return 0;
        }
    }
}

static int download_api(const char *url, char *api_path, uint32_t capacity)
{
    struct download_state state;
    struct reliefos_gui_app_event event;
    struct reliefos_ui_surface ui;
    char status_state = 0;
    char *argv[6];
    int worker_status;
    uint8_t download_complete = 0;
    memset(&state, 0, sizeof(state));
    build_download_path(state.output_path, sizeof(state.output_path));
    build_download_status_path(state.status_path, sizeof(state.status_path));
    if (!state.output_path[0] || strlen(state.output_path) >= capacity) {
        return 0;
    }
    unlink(state.status_path);
    state.window_id = reliefos_gui_create_app_window_ex(
        T("API Installer"),
        T("Downloading application"),
        WIZARD_W, WIZARD_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (state.window_id <= 0) {
        return 0;
    }
    copy_text(state.status, sizeof(state.status),
              T("Preparing download..."));
    reliefos_ui_bind(&ui, wizard_pixels, WIZARD_W, WIZARD_H, WIZARD_W);
    draw_download_page(&ui, &state);
    reliefos_gui_present_window((uint32_t)state.window_id, WIZARD_W, WIZARD_H,
                              WIZARD_W, wizard_pixels);
    argv[0] = APIAPP_PATH;
    argv[1] = "--download-worker";
    argv[2] = (char *)url;
    argv[3] = state.output_path;
    argv[4] = state.status_path;
    argv[5] = 0;
    state.worker_pid = reliefos_spawn_argv(APIAPP_PATH, argv);
    if (state.worker_pid < 0) {
        copy_text(state.status, sizeof(state.status),
                  T("Could not start download"));
        (void)wait_for_download_close(&state, &ui);
        reliefos_gui_destroy_app_window((uint32_t)state.window_id);
        return 0;
    }

    for (;;) {
        uint32_t received;
        uint32_t total;
        if (read_download_status(state.status_path, &status_state, &received,
                                 &total) == 0) {
            state.received = received;
            state.total = total;
            if (status_state == 'R') {
                copy_text(state.status, sizeof(state.status),
                          total ? T("Downloading...")
                                : T("Connecting..."));
            } else if (status_state == 'D') {
                download_complete = 1;
                copy_text(state.status, sizeof(state.status),
                          T("Finalizing download..."));
            } else if (status_state == 'F') {
                copy_text(state.status, sizeof(state.status),
                          T("Download failed"));
            }
        }
        if (download_worker_exited(state.worker_pid)) {
            worker_status = 0;
            (void)wait4(state.worker_pid, &worker_status, 0, 0);
            state.worker_pid = 0;
            if (download_complete) {
                copy_text(api_path, capacity, state.output_path);
                unlink(state.status_path);
                reliefos_gui_destroy_app_window((uint32_t)state.window_id);
                return 1;
            }
            break;
        }
        draw_download_page(&ui, &state);
        reliefos_gui_present_window((uint32_t)state.window_id, WIZARD_W,
                                  WIZARD_H, WIZARD_W, wizard_pixels);
        event.window_id = (uint32_t)state.window_id;
        if (reliefos_gui_wait_app_event(&event, 50U) > 0 &&
            (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE ||
             (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
              (event.buttons & 1U) &&
              hit(event.x, event.y, WIZARD_W - 120U, WIZARD_H - 52U,
                  88U, RELIEFOS_UI_BUTTON_H)))) {
            state.cancelled = 1;
            break;
        }
    }
    if (state.worker_pid > 0) {
        if (state.cancelled) {
            (void)reliefos_task_kill((uint32_t)state.worker_pid);
        }
    }
    cleanup_download(&state);
    copy_text(state.status, sizeof(state.status),
              state.cancelled ? T("Download cancelled")
                              : T("Download failed"));
    (void)wait_for_download_close(&state, &ui);
    reliefos_gui_destroy_app_window((uint32_t)state.window_id);
    return 0;
}

static void draw_welcome_page(struct reliefos_ui_surface *ui,
                              const struct reliefos_api_info *info)
{
    reliefos_ui_rect(ui, 0, 0, WIZARD_W, WIZARD_H, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 24, T("Application Installer"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 64,
                   T("Ready to install:"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 32, 88, WIZARD_W - 64U, info->name,
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    if (info->version[0]) {
        reliefos_ui_text(ui, 32, 112,
                       T("Version:"),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
        reliefos_ui_text(ui, 120, 112, info->version,
                       RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    }
    reliefos_ui_text(ui, 32, 144,
                   T("Install location:"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 32, 168, WIZARD_W - 64U,
                           info->default_path,
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_button(ui, WIZARD_W - 120U, WIZARD_H - 52U, 88U,
                     RELIEFOS_UI_BUTTON_H,
                     T("Next >"), 0);
    reliefos_ui_button(ui, WIZARD_W - 216U, WIZARD_H - 52U, 88U,
                     RELIEFOS_UI_BUTTON_H,
                     T("Cancel"), 0);
}

static void draw_install_page(struct reliefos_ui_surface *ui,
                              struct install_state *state)
{
    reliefos_ui_rect(ui, 0, 0, WIZARD_W, WIZARD_H, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 24,
                   T("Install Options"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 64,
                   T("Install path:"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 32, 88, WIZARD_W - 64U, state->install_path,
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_checkbox(ui, 32, 132,
                       T("Create desktop shortcut"),
                       (int)state->create_shortcut, 0);
    reliefos_ui_button(ui, WIZARD_W - 120U, WIZARD_H - 52U, 88U,
                     RELIEFOS_UI_BUTTON_H,
                     T("Install"), 0);
    reliefos_ui_button(ui, WIZARD_W - 216U, WIZARD_H - 52U, 88U,
                     RELIEFOS_UI_BUTTON_H,
                     T("Cancel"), 0);
    reliefos_ui_button(ui, WIZARD_W - 312U, WIZARD_H - 52U, 88U,
                     RELIEFOS_UI_BUTTON_H,
                     T("< Back"), 0);
}

static void draw_finish_page(struct reliefos_ui_surface *ui, int success,
                             const char *name)
{
    reliefos_ui_rect(ui, 0, 0, WIZARD_W, WIZARD_H, RELIEFOS_UI_WHITE);
    if (success) {
        reliefos_ui_text(ui, 32, 24,
                       T("Installation Complete"),
                       RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        reliefos_ui_text(ui, 32, 64,
                       T("Successfully installed:"),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
        reliefos_ui_text_clipped(ui, 32, 88, WIZARD_W - 64U, name,
                               RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    } else {
        reliefos_ui_text(ui, 32, 24,
                       T("Installation Failed"),
                       RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
        reliefos_ui_text(ui, 32, 64,
                       T("Could not install:"),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
        reliefos_ui_text_clipped(ui, 32, 88, WIZARD_W - 64U, name,
                               RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    }
    reliefos_ui_button(ui, WIZARD_W - 120U, WIZARD_H - 52U, 88U,
                     RELIEFOS_UI_BUTTON_H,
                     T("Close"), 0);
}

static int run_wizard(const char *api_path)
{
    struct install_state state;
    struct reliefos_gui_app_event event;
    struct reliefos_ui_surface ui;
    int window_id;
    int done = 0;
    int result = 0;
    int step = 0;
    uint32_t api_path_len;

    memset(&state, 0, sizeof(state));
    api_path_len = (uint32_t)strlen(api_path);
    if (api_path_len >= sizeof(state.api_path)) {
        return 1;
    }
    memcpy(state.api_path, api_path, api_path_len + 1U);

    if (!reliefos_api_parse_info(api_path, &state.info)) {
        return 1;
    }
    memcpy(state.install_path, state.info.default_path,
           strlen(state.info.default_path) + 1U);
    state.create_shortcut = state.info.desktop_shortcut;

    window_id = reliefos_gui_create_app_window_ex(
        T("API Installer"),
        T("API Installer"),
        WIZARD_W, WIZARD_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        return 1;
    }
    reliefos_ui_bind(&ui, wizard_pixels, WIZARD_W, WIZARD_H, WIZARD_W);

    while (!done) {
        reliefos_ui_rect(&ui, 0, 0, WIZARD_W, WIZARD_H, RELIEFOS_UI_WHITE);

        if (step == 0) {
            draw_welcome_page(&ui, &state.info);
        } else if (step == 1) {
            draw_install_page(&ui, &state);
        } else {
            draw_finish_page(&ui, result, state.info.name);
        }
        reliefos_gui_present_window((uint32_t)window_id, WIZARD_W, WIZARD_H,
                                  WIZARD_W, wizard_pixels);

        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                break;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
                (event.buttons & 1U)) {
                if (step == 0) {
                    if (event.x >= (int32_t)(WIZARD_W - 120) &&
                        event.x < (int32_t)(WIZARD_W - 32) &&
                        event.y >= (int32_t)(WIZARD_H - 52) &&
                        event.y < (int32_t)(WIZARD_H - 52 + RELIEFOS_UI_BUTTON_H)) {
                        step = 1;
                    }
                    if (event.x >= (int32_t)(WIZARD_W - 216) &&
                        event.x < (int32_t)(WIZARD_W - 128) &&
                        event.y >= (int32_t)(WIZARD_H - 52) &&
                        event.y < (int32_t)(WIZARD_H - 52 + RELIEFOS_UI_BUTTON_H)) {
                        done = 1;
                    }
                } else if (step == 1) {
                    if (event.x >= (int32_t)(WIZARD_W - 120) &&
                        event.x < (int32_t)(WIZARD_W - 32) &&
                        event.y >= (int32_t)(WIZARD_H - 52) &&
                        event.y < (int32_t)(WIZARD_H - 52 + RELIEFOS_UI_BUTTON_H)) {
                        result = install_api_with_progress(
                            window_id, state.api_path, state.install_path,
                            state.create_shortcut);
                        step = 2;
                    }
                    if (event.x >= (int32_t)(WIZARD_W - 216) &&
                        event.x < (int32_t)(WIZARD_W - 128) &&
                        event.y >= (int32_t)(WIZARD_H - 52) &&
                        event.y < (int32_t)(WIZARD_H - 52 + RELIEFOS_UI_BUTTON_H)) {
                        done = 1;
                    }
                    if (event.x >= (int32_t)(WIZARD_W - 312) &&
                        event.x < (int32_t)(WIZARD_W - 224) &&
                        event.y >= (int32_t)(WIZARD_H - 52) &&
                        event.y < (int32_t)(WIZARD_H - 52 + RELIEFOS_UI_BUTTON_H)) {
                        step = 0;
                    }
                    if (event.x >= 32 &&
                        event.x < 200 &&
                        event.y >= 132 &&
                        event.y < 152) {
                        state.create_shortcut =
                            state.create_shortcut ? 0U : 1U;
                    }
                } else {
                    if (event.x >= (int32_t)(WIZARD_W - 120) &&
                        event.x < (int32_t)(WIZARD_W - 32) &&
                        event.y >= (int32_t)(WIZARD_H - 52) &&
                        event.y < (int32_t)(WIZARD_H - 52 + RELIEFOS_UI_BUTTON_H)) {
                        done = 1;
                    }
                }
            }
        }
    }
    reliefos_gui_destroy_app_window((uint32_t)window_id);
    return result ? 0 : 1;
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    char downloaded_path[RELIEFOS_API_PATH_MAX];
    if (argc == 5 && argv && argv[1] && argv[2] && argv[3] && argv[4] &&
        strcmp(argv[1], "--download-worker") == 0) {
        return run_download_worker(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 && argv && argv[1] && argv[2] && argv[3] && argv[4] &&
        strcmp(argv[1], "--install-worker") == 0 &&
        (!strcmp(argv[4], "0") || !strcmp(argv[4], "1"))) {
        return run_install_worker(argv[2], argv[3],
                                  strcmp(argv[4], "1") == 0);
    }
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        return 1;
    }
    if (starts_with_ignore_case(argv[1], "http://") ||
        starts_with_ignore_case(argv[1], "https://")) {
        if (!download_api(argv[1], downloaded_path, sizeof(downloaded_path))) {
            return 1;
        }
        return run_wizard(downloaded_path);
    }
    return run_wizard(argv[1]);
}
