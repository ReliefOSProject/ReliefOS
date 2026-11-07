#include <reliefos/auth.h>
#include <reliefos/xdg.h>
#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <reliefos/http.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>

#define DOWNLOAD_W 680U
#define DOWNLOAD_H 300U
#define URL_X 84U
#define URL_Y 38U
#define URL_W 470U
#define BUTTON_X 568U
#define BUTTON_Y 38U
#define BUTTON_W 86U
#define T(s) gettext(s)

static uint32_t pixels[DOWNLOAD_W * DOWNLOAD_H];
static char url_input[RELIEFOS_HTTP_URL_LEN] = "http://example.com/";
static char status_text[160] = "Ready";
static char target_path[RELIEFOS_FS_PATH_LEN] = "";
static char detail_text[192] = "Enter an http:// or https:// URL or open a download link from Browser.";
static char body[RELIEFOS_HTTP_BODY_MAX + 1U];
static char headers[RELIEFOS_HTTP_HEADER_MAX + 1U];
static struct reliefos_ui_edit_state url_edit;
static uint32_t progress_value;
static uint8_t busy;
static uint8_t done;
static uint8_t failed;

static uint32_t text_len(const char *text)
{
    uint32_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n;
}

static char ascii_tolower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int starts_with_ignore_case(const char *text, const char *prefix)
{
    uint32_t i = 0;
    if (!text || !prefix) {
        return 0;
    }
    while (prefix[i]) {
        if (ascii_tolower(text[i]) != ascii_tolower(prefix[i])) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1U < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void append_char(char *dst, uint32_t *pos, uint32_t cap, char ch)
{
    if (dst && pos && *pos + 1U < cap) {
        dst[*pos] = ch;
        ++(*pos);
        dst[*pos] = 0;
    }
}

static void append_text(char *dst, uint32_t *pos, uint32_t cap,
                        const char *src)
{
    while (src && *src) {
        append_char(dst, pos, cap, *src++);
    }
}

static void append_u32(char *dst, uint32_t *pos, uint32_t cap, uint32_t value)
{
    char tmp[12];
    uint32_t n = 0;
    if (value == 0) {
        append_char(dst, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (n) {
        append_char(dst, pos, cap, tmp[--n]);
    }
}

static void path_parent(char *dst, uint32_t cap, const char *path)
{
    uint32_t len;
    copy_text(dst, cap, path);
    len = text_len(dst);
    while (len > 1U && dst[len - 1U] != '/') {
        dst[--len] = 0;
    }
    if (len > 1U) {
        dst[len - 1U] = 0;
    }
}

static const char *url_filename(const char *url)
{
    const char *base = url;
    const char *p = url;
    if (!url) {
        return "";
    }
    while (*p && *p != '?' && *p != '#') {
        if (*p == '/') {
            base = p + 1;
        }
        ++p;
    }
    return base;
}

static void sanitize_filename(char *dst, uint32_t cap, const char *src)
{
    uint32_t pos = 0;
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = 0;
    while (src && src[i] && src[i] != '?' && src[i] != '#' && pos + 1U < cap) {
        char ch = src[i++];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_') {
            append_char(dst, &pos, cap, ch);
        } else {
            append_char(dst, &pos, cap, '_');
        }
    }
    if (!dst[0] || dst[0] == '.') {
        copy_text(dst, cap, "download.bin");
    }
}

static void build_download_dir(char *dst, uint32_t cap)
{
    if (reliefos_xdg_user_dir("DOWNLOAD",dst,cap)<0) copy_text(dst,cap,"/tmp");
}

static void build_child_path(char *dst, uint32_t cap, const char *dir,
                             const char *name)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_text(dst, &pos, cap, dir);
    if (dir && dir[0] && dir[text_len(dir) - 1U] != '/') {
        append_char(dst, &pos, cap, '/');
    }
    append_text(dst, &pos, cap, name);
}

static void build_numbered_name(char *dst, uint32_t cap, const char *name,
                                uint32_t number)
{
    uint32_t dot = 0;
    uint32_t len = text_len(name);
    uint32_t pos = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (name[i] == '.') {
            dot = i;
        }
    }
    if (!dot) {
        dot = len;
    }
    dst[0] = 0;
    for (uint32_t i = 0; i < dot; ++i) {
        append_char(dst, &pos, cap, name[i]);
    }
    append_text(dst, &pos, cap, "-");
    append_u32(dst, &pos, cap, number);
    for (uint32_t i = dot; i < len; ++i) {
        append_char(dst, &pos, cap, name[i]);
    }
}

static int choose_target_path(char *dst, uint32_t cap, const char *url)
{
    char dir[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    char numbered[RELIEFOS_FS_NAME_LEN];
    struct reliefos_stat st;
    build_download_dir(dir, sizeof(dir));
    (void)mkdir(dir, 0700);
    sanitize_filename(name, sizeof(name), url_filename(url));
    for (uint32_t i = 0; i < 100U; ++i) {
        if (i == 0) {
            build_child_path(dst, cap, dir, name);
        } else {
            build_numbered_name(numbered, sizeof(numbered), name, i + 1U);
            build_child_path(dst, cap, dir, numbered);
        }
        if (reliefos_stat_legacy(dst, &st) < 0) {
            return 0;
        }
    }
    return -1;
}

static const char *net_status_name(uint32_t status)
{
    switch (status) {
    case RELIEFOS_NET_STATUS_OK:
        return T("Succeeded");
    case RELIEFOS_NET_STATUS_TCP_TIMEOUT:
        return T("TCP timeout");
    case RELIEFOS_NET_STATUS_DNS_FAILED:
        return T("DNS failed");
    case RELIEFOS_NET_STATUS_DNS_NO_ANSWER:
        return T("No DNS answer");
    case RELIEFOS_NET_STATUS_PROTOCOL_UNSUPPORTED:
        return T("Protocol unsupported");
    case RELIEFOS_NET_STATUS_TLS_FAILED:
        return T("TLS verification failed");
    default:
        return T("Network failed");
    }
}

static void set_detail_done(const struct reliefos_http_response *response)
{
    uint32_t pos = 0;
    detail_text[0] = 0;
    append_text(detail_text, &pos, sizeof(detail_text), T("Saved "));
    append_u32(detail_text, &pos, sizeof(detail_text), response->body_len);
    append_text(detail_text, &pos, sizeof(detail_text), T(" bytes to "));
    append_text(detail_text, &pos, sizeof(detail_text), target_path);
}

static int save_body(const char *path, const char *data, uint32_t len)
{
    int fd = open(path, RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0666);
    long wrote;
    if (fd < 0) {
        return fd;
    }
    wrote = write(fd, data, len);
    close(fd);
    if (wrote < 0) {
        return (int)wrote;
    }
    return (uint32_t)wrote == len ? 0 : -1;
}

static void perform_download(void)
{
    struct reliefos_http_response response;
    int ret;
    busy = 1;
    done = 0;
    failed = 0;
    progress_value = 10;
    copy_text(status_text, sizeof(status_text), T("Connecting..."));
    copy_text(detail_text, sizeof(detail_text), url_input);
    body[0] = 0;
    headers[0] = 0;
    if (!starts_with_ignore_case(url_input, "http://") &&
        !starts_with_ignore_case(url_input, "https://")) {
        failed = 1;
        busy = 0;
        progress_value = 0;
        copy_text(status_text, sizeof(status_text),
                  T("Only http:// and https:// downloads are supported."));
        return;
    }
    ret = reliefos_http_get(url_input, RELIEFOS_HTTP_DEFAULT_TIMEOUT_MS,
                          body, sizeof(body), headers, sizeof(headers),
                          &response);
    progress_value = 72;
    if (ret < 0 || response.net_status != RELIEFOS_NET_STATUS_OK) {
        failed = 1;
        busy = 0;
        copy_text(status_text, sizeof(status_text),
                  ret < 0 ? T("HTTP client failed")
                          : net_status_name(response.net_status));
        return;
    }
    if (response.http_status < 200U || response.http_status >= 300U) {
        uint32_t pos = 0;
        failed = 1;
        busy = 0;
        status_text[0] = 0;
        append_text(status_text, &pos, sizeof(status_text), T("HTTP status "));
        append_u32(status_text, &pos, sizeof(status_text), response.http_status);
        return;
    }
    if (response.flags & RELIEFOS_HTTP_FLAG_TRUNCATED) {
        failed = 1;
        busy = 0;
        copy_text(status_text, sizeof(status_text),
                  T("Download is too large for v1 client."));
        return;
    }
    if (choose_target_path(target_path, sizeof(target_path),
                           response.final_url[0] ? response.final_url : url_input) < 0) {
        failed = 1;
        busy = 0;
        copy_text(status_text, sizeof(status_text),
                  T("Could not choose target file."));
        return;
    }
    path_parent(detail_text, sizeof(detail_text), target_path);
    ret = save_body(target_path, body, response.body_len);
    if (ret < 0) {
        failed = 1;
        busy = 0;
        copy_text(status_text, sizeof(status_text),
                  T("Could not save file."));
        return;
    }
    progress_value = 100;
    done = 1;
    busy = 0;
    copy_text(status_text, sizeof(status_text), T("Download complete"));
    set_detail_done(&response);
}

static void draw_downloadmgr(struct reliefos_ui_surface *ui)
{
    uint32_t state_color = failed ? 0x00b03030U : (done ? 0x00108040U : RELIEFOS_UI_DARK);
    reliefos_ui_rect(ui, 0, 0, DOWNLOAD_W, DOWNLOAD_H, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 24, 14,
                   T("Web downloads are saved to the current user's Downloads folder."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 24, URL_Y + 4U, T("URL:"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_edit_state_draw(ui, URL_X, URL_Y, URL_W, &url_edit, busy ? RELIEFOS_UI_EDIT_DISABLED : 0);
    reliefos_ui_button(ui, BUTTON_X, BUTTON_Y, BUTTON_W, RELIEFOS_UI_BUTTON_H,
                     T("Download"), busy ? RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_text(ui, 24, 88, T("Progress"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_progress(ui, 104, 84, DOWNLOAD_W - 142, 22,
                       progress_value, 100);
    reliefos_ui_text_clipped(ui, 24, 130, DOWNLOAD_W - 48,
                           status_text, state_color, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 24, 158, DOWNLOAD_W - 48,
                           detail_text, RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    if (target_path[0]) {
        reliefos_ui_text_clipped(ui, 24, 188, DOWNLOAD_W - 48,
                               target_path, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    }
    reliefos_ui_statusbar(ui, DOWNLOAD_H - 28, 28, status_text);
}

static void present(int window_id, struct reliefos_ui_surface *ui)
{
    draw_downloadmgr(ui);
    reliefos_gui_present_window((uint32_t)window_id, DOWNLOAD_W, DOWNLOAD_H,
                              DOWNLOAD_W, pixels);
}

static int hit_rect(int32_t px, int32_t py, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h)
{
    return px >= (int32_t)x && py >= (int32_t)y &&
           px < (int32_t)(x + w) && py < (int32_t)(y + h);
}

int main(int argc, char **argv, char **envp)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    uint8_t auto_start = 0;
    (void)envp;
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        copy_text(url_input, sizeof(url_input), argv[1]);
        auto_start = 1;
    }
    window_id = reliefos_gui_create_app_window_ex(T("Download Manager"),
                                                T("Web downloads"),
                                                DOWNLOAD_W, DOWNLOAD_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[downloadmgr.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, DOWNLOAD_W, DOWNLOAD_H, DOWNLOAD_W);
    reliefos_ui_edit_state_init(&url_edit, url_input, sizeof(url_input));
    url_edit.focused = !auto_start;
    present(window_id, &ui);
    if (auto_start) {
        perform_download();
        present(window_id, &ui);
    }
    for (;;) {
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
                (event.buttons & 1U)) {
                if (reliefos_ui_edit_state_handle_mouse(&url_edit, event.x, event.y,
                                                      URL_X, URL_Y, URL_W,
                                                      event.buttons)) {
                    present(window_id, &ui);
                    continue;
                }
                if (!busy && hit_rect(event.x, event.y, BUTTON_X, BUTTON_Y,
                                      BUTTON_W, RELIEFOS_UI_BUTTON_H)) {
                    perform_download();
                    present(window_id, &ui);
                    continue;
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
                event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                if (event.pressed && event.keycode == 1U) {
                    return 0;
                }
                if (event.pressed && event.keycode == RELIEFOS_KEY_ENTER && !busy) {
                    perform_download();
                    present(window_id, &ui);
                } else if (reliefos_ui_edit_state_handle_key(&url_edit,
                                                           event.keycode,
                                                           event.pressed)) {
                    present(window_id, &ui);
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_FOCUS ||
                event.type == RELIEFOS_GUI_APP_EVENT_RESIZE) {
                present(window_id, &ui);
            }
        } else {
            sleep_ms(10);
        }
    }
}
