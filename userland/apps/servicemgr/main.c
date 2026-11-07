#include <reliefos/openrc.h>
#include <errno.h>
#include <string.h>
#include <reliefos/auth.h>
#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <reliefos/layout.h>

#define SERVICEMGR_W 780U
#define SERVICEMGR_H 430U
#define SERVICEMGR_ROWS 4U
#define SERVICEMGR_CONFIG_MAX 512U
#define SERVICEMGR_STATE_MAX 1024U
#define SERVICEMGR_ROW_Y 60U
#define SERVICEMGR_ROW_H 52U
#define T(s) gettext(s)
#define N_(s) (s)

struct service_row {
    const char *key;
    const char *name;
    const char *detail;
    uint8_t enabled;
    uint8_t locked;
    char state[16];
    char state_detail[96];
    uint32_t pid;
};

static uint32_t pixels[SERVICEMGR_W * SERVICEMGR_H];
static struct reliefos_user_info current_user;
static uint8_t can_manage;
static int32_t selected_row = 1;
static char status_text[180] = "Ready";
static unsigned long last_state_refresh_ms;

static struct service_row service_rows[SERVICEMGR_ROWS] = {
    {"reliefos-audio", N_("Audio"), N_("ALSA state"), 1, 0, "unknown", "", 0},
    {"reliefos-dhcp", N_("DHCP"), N_("BusyBox udhcpc"), 1, 0, "unknown", "", 0},
    {"reliefos-device", N_("Devices"), N_("ReliefOS device protocol"), 1, 0, "unknown", "", 0},
    {"reliefos-ntp", N_("Time sync"), N_("BusyBox ntpd"), 1, 0, "unknown", "", 0},
};

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

static uint32_t text_len(const char *text)
{
    uint32_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n;
}

static int text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static void refresh_user(void)
{
    current_user = (struct reliefos_user_info){0};
    can_manage = 0;
    if (reliefos_auth_current(&current_user) == 0 &&
        current_user.role == RELIEFOS_AUTH_ROLE_ADMIN) {
        can_manage = 1;
    }
}

static void load_config(void)
{
    for (unsigned i = 0; i < SERVICEMGR_ROWS; ++i)
        service_rows[i].enabled = reliefos_openrc_enabled(service_rows[i].key) == 1;
}

/* Keep authentication and OpenRC waits outside the GUI event loop. */
static int rc_child, rc_kind;
static unsigned rc_row;
static const char *rc_action;
static void load_state(uint8_t quiet);

static void save_config(void)
{
    if (!can_manage || rc_child) return;
    for (unsigned i = 0; i < SERVICEMGR_ROWS; ++i) {
        if (reliefos_openrc_enabled(service_rows[i].key) == service_rows[i].enabled) continue;
        rc_row = i; rc_kind = 3;
        rc_action = service_rows[i].enabled ? "enable" : "disable";
        rc_child = reliefos_openrc_spawn(service_rows[i].key, rc_action);
        if (rc_child < 0) { rc_child = 0; copy_text(status_text, sizeof(status_text), "OpenRC worker failed"); }
        else copy_text(status_text, sizeof(status_text), T("Updating runlevel..."));
        return;
    }
    copy_text(status_text, sizeof(status_text), T("Default runlevel updated"));
}

static void load_state(uint8_t quiet)
{
    if (rc_child) return;
    rc_row = 0; rc_kind = 1; rc_action = "status";
    rc_child = reliefos_openrc_spawn(service_rows[0].key, "status");
    if (rc_child < 0) { rc_child = 0; copy_text(status_text, sizeof(status_text), "OpenRC worker failed"); }
    else if (!quiet) copy_text(status_text, sizeof(status_text), T("Refreshing OpenRC..."));
}

static void write_command(const char *action, uint32_t row)
{
    if (!can_manage || row >= SERVICEMGR_ROWS || rc_child) return;
    rc_row = row; rc_kind = 2; rc_action = action;
    rc_child = reliefos_openrc_spawn(service_rows[row].key, action);
    if (rc_child < 0) rc_child = 0;
    snprintf(status_text, sizeof(status_text), "OpenRC %s %s: %s", service_rows[row].key, action,
             rc_child ? "pending" : "worker failed");
}

static int poll_openrc(void)
{
    if (!rc_child) return 0;
    int result = 125, ready = reliefos_openrc_poll(rc_child, &result);
    if (!ready) return 0;
    rc_child = 0;
    if (ready < 0) result = 125;
    if (rc_kind == 1) {
        copy_text(service_rows[rc_row].state, sizeof(service_rows[rc_row].state),
                  result == 0 ? "running" : result == 3 ? "stopped" : "failed");
        snprintf(service_rows[rc_row].state_detail, sizeof(service_rows[rc_row].state_detail),
                 "OpenRC exit=%d; default=%s", result,
                 reliefos_openrc_enabled(service_rows[rc_row].key) == 1 ? "yes" : "no");
        if (++rc_row < SERVICEMGR_ROWS) {
            rc_child = reliefos_openrc_spawn(service_rows[rc_row].key, "status");
            if (rc_child < 0) rc_child = 0;
        }
    } else {
        snprintf(status_text, sizeof(status_text), "OpenRC %s %s: exit=%d", service_rows[rc_row].key, rc_action, result);
        if (rc_kind == 3 && !result) save_config();
        else if (result) load_config();
    }
    return 1;
}

static const char *localized_state(const char *state)
{
    if (text_eq(state, "running")) {
        return T("Running");
    }
    if (text_eq(state, "stopped")) {
        return T("Stopped");
    }
    if (text_eq(state, "failed")) {
        return T("Failed");
    }
    if (text_eq(state, "starting")) {
        return T("Starting");
    }
    return T("Unknown");
}

static void draw_servicemgr(struct reliefos_ui_surface *ui)
{
    reliefos_ui_rect(ui, 0, 0, SERVICEMGR_W, SERVICEMGR_H, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 24, 16,
                   can_manage
                       ? T("View service runtime state and control startup services.")
                       : T("Runtime state is visible. Administrator rights are required to control services."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 36, 42, T("Service"), RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 300, 42, T("State"), RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 408, 42, "PID", RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 462, 42, T("Detail"), RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);

    for (uint32_t i = 0; i < SERVICEMGR_ROWS; ++i) {
        char pid_text[16];
        uint32_t pos = 0;
        uint32_t y = SERVICEMGR_ROW_Y + i * SERVICEMGR_ROW_H;
        uint32_t row_selected = selected_row == (int32_t)i;
        uint32_t row_bg = row_selected ? reliefos_ui_color(RELIEFOS_UI_COLOR_SELECTION)
                                       : RELIEFOS_UI_WHITE;
        uint32_t flags = (!can_manage || service_rows[i].locked)
                             ? RELIEFOS_UI_BUTTON_DISABLED
                             : 0;
        pid_text[0] = 0;
        (void)pos;
        copy_text(pid_text, sizeof(pid_text), "—");
        reliefos_ui_panel(ui, 24, y, SERVICEMGR_W - 48U, 44U, row_bg);
        reliefos_ui_checkbox(ui, 36, y + 12U,
                           T(service_rows[i].name),
                           service_rows[i].enabled, flags);
        reliefos_ui_text_clipped(ui, 156, y + 14U, 126,
                               T(service_rows[i].detail),
                               RELIEFOS_UI_DARK, row_bg);
        reliefos_ui_text_clipped(ui, 300, y + 14U, 92,
                               localized_state(service_rows[i].state),
                               text_eq(service_rows[i].state, "failed")
                                   ? 0x00c00000
                                   : RELIEFOS_UI_BLACK,
                               row_bg);
        reliefos_ui_text_clipped(ui, 408, y + 14U, 42, pid_text,
                               RELIEFOS_UI_DARK, row_bg);
        reliefos_ui_text_clipped(ui, 462, y + 14U, SERVICEMGR_W - 498U,
                               service_rows[i].state_detail,
                               RELIEFOS_UI_BLACK, row_bg);
    }
    reliefos_ui_button(ui, 24, SERVICEMGR_H - 66U, 92U, RELIEFOS_UI_BUTTON_H,
                     T("Refresh"), 0);
    reliefos_ui_button(ui, 126, SERVICEMGR_H - 66U, 92U, RELIEFOS_UI_BUTTON_H,
                     T("Save"),
                     can_manage ? 0 : RELIEFOS_UI_BUTTON_DISABLED);
    reliefos_ui_button(ui, 240, SERVICEMGR_H - 66U, 92U, RELIEFOS_UI_BUTTON_H,
                     T("Start"),
                     can_manage && selected_row >= 0 &&
                             !service_rows[selected_row].locked
                         ? 0
                         : RELIEFOS_UI_BUTTON_DISABLED);
    reliefos_ui_button(ui, 342, SERVICEMGR_H - 66U, 92U, RELIEFOS_UI_BUTTON_H,
                     T("Stop"),
                     can_manage && selected_row >= 0 &&
                             !service_rows[selected_row].locked
                         ? 0
                         : RELIEFOS_UI_BUTTON_DISABLED);
    reliefos_ui_button(ui, 444, SERVICEMGR_H - 66U, 104U, RELIEFOS_UI_BUTTON_H,
                     T("Restart"),
                     can_manage && selected_row >= 0 &&
                             !service_rows[selected_row].locked
                         ? 0
                         : RELIEFOS_UI_BUTTON_DISABLED);
    reliefos_ui_statusbar(ui, SERVICEMGR_H - 28U, 28U, status_text);
}

static void present(int window_id, struct reliefos_ui_surface *ui)
{
    draw_servicemgr(ui);
    reliefos_gui_present_window((uint32_t)window_id, SERVICEMGR_W,
                              SERVICEMGR_H, SERVICEMGR_W, pixels);
}

static int hit_rect(int32_t px, int32_t py, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h)
{
    return px >= (int32_t)x && py >= (int32_t)y &&
           px < (int32_t)(x + w) && py < (int32_t)(y + h);
}

static void handle_click(int32_t x, int32_t y)
{
    if (rc_child) return;
    for (uint32_t i = 0; i < SERVICEMGR_ROWS; ++i) {
        uint32_t row_y = SERVICEMGR_ROW_Y + i * SERVICEMGR_ROW_H;
        if (hit_rect(x, y, 24, row_y, SERVICEMGR_W - 48U, 44U)) {
            selected_row = (int32_t)i;
        }
        if (can_manage && !service_rows[i].locked &&
            hit_rect(x, y, 36, row_y + 8U, 110U, 30U)) {
            selected_row = (int32_t)i;
            service_rows[i].enabled = service_rows[i].enabled ? 0 : 1;
            copy_text(status_text, sizeof(status_text),
                      T("Service setting changed"));
            return;
        }
    }
    if (hit_rect(x, y, 24, SERVICEMGR_H - 66U, 92U, RELIEFOS_UI_BUTTON_H)) {
        load_config();
        load_state(0);
    } else if (hit_rect(x, y, 126, SERVICEMGR_H - 66U,
                        92U, RELIEFOS_UI_BUTTON_H)) {
        save_config();
    } else if (hit_rect(x, y, 240, SERVICEMGR_H - 66U,
                        92U, RELIEFOS_UI_BUTTON_H) &&
               selected_row >= 0) {
        write_command("start", (uint32_t)selected_row);
    } else if (hit_rect(x, y, 342, SERVICEMGR_H - 66U,
                        92U, RELIEFOS_UI_BUTTON_H) &&
               selected_row >= 0) {
        write_command("stop", (uint32_t)selected_row);
    } else if (hit_rect(x, y, 444, SERVICEMGR_H - 66U,
                        104U, RELIEFOS_UI_BUTTON_H) &&
               selected_row >= 0) {
        write_command("restart", (uint32_t)selected_row);
    }
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    puts("[servicemgr.elf] service manager starting");
    refresh_user();
    load_config();
    load_state(1);
    window_id = reliefos_gui_create_app_window_ex(T("Service Manager"),
                                                T("Services"),
                                                SERVICEMGR_W, SERVICEMGR_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[servicemgr.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, SERVICEMGR_W, SERVICEMGR_H, SERVICEMGR_W);
    present(window_id, &ui);
    for (;;) {
        if (poll_openrc()) present(window_id, &ui);
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON &&
                (event.buttons & 1U)) {
                handle_click(event.x, event.y);
                present(window_id, &ui);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN &&
                event.pressed && event.keycode == 1U) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_FOCUS ||
                event.type == RELIEFOS_GUI_APP_EVENT_RESIZE) {
                refresh_user();
                load_state(1);
                present(window_id, &ui);
            }
        } else {
            unsigned long now = reliefos_uptime_ms();
            if (now - last_state_refresh_ms >= 1000UL) {
                last_state_refresh_ms = now;
                load_state(1);
                present(window_id, &ui);
            }
            sleep_ms(20);
        }
    }
}
