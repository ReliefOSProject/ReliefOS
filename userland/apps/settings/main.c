#include <reliefos/openrc.h>
#include <pwd.h>
#include <unistd.h>
#include <reliefos/auth.h>
#include <reliefos/app.h>
#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include "../locale_settings.h"
#include <locale.h>
#include <reliefos/layout.h>
#include "sound_page.h"
#include <reliefos/environment.h>
#include <reliefos/text_input.h>
#include <reliefos/launch.h>
#include <reliefos/license.h>
#include <reliefos/psf_font.h>
#include <reliefos/startup.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <reliefos/layout.h>

#define SETTINGS_W 720
#define SETTINGS_H 470
#define SETTINGS_DROPDOWN_ROW_H 28
#define SETTINGS_MODE_COUNT 5
#define SETTINGS_SCALE_COUNT 3
#define SETTINGS_TAB_COUNT 8
#define SETTINGS_USER_ROWS 7
#define SETTINGS_ASSOC_ROWS 6
#define SETTINGS_SERVICE_ROWS 5
#define SETTINGS_WALLPAPER_MAX_W 1280U
#define SETTINGS_WALLPAPER_MAX_H 720U
#define SETTINGS_WALLPAPER_BMP_MAX_BYTES (SETTINGS_WALLPAPER_MAX_W * SETTINGS_WALLPAPER_MAX_H * 4U + 128U)
#define SETTINGS_DEFAULT_WALLPAPER_PATH RELIEFOS_PATH_WALLPAPER_BMP
#define SETTINGS_TAB_Y 14
#define SETTINGS_BODY_Y 44
#define SETTINGS_SERVICES_PATH RELIEFOS_PATH_TASKBAR_CFG
#define SETTINGS_SERVICES_CONFIG_MAX 512U
#define SETTINGS_INPUTM_CONFIG_MAX 2048U
#define SETTINGS_INPUTM_ROWS (TEXT_INPUT_MAX_PROVIDERS + 1U)
#define SETTINGS_INPUTM_OPTION_COUNT 4U
#define SETTINGS_INPUTM_OPTION_KEY_LEN 64U
#define SETTINGS_INPUTM_OPTION_LABEL_LEN 64U
#define SETTINGS_KEY_ESCAPE 1U
#define T(s) gettext(s)
#define N_(s) (s)

enum {
    PAGE_DISPLAY = 0,
    PAGE_PERSONALIZATION = 1,
    PAGE_USERS = 2,
    PAGE_ASSOC = 3,
    PAGE_SERVICES = 4,
    PAGE_ACTIVATION = 5,
    PAGE_INPUT_METHODS = 6,
    PAGE_SOUND = 7,
};

enum {
    DROP_NONE = 0,
    DROP_RESOLUTION = 1,
    DROP_SCALE = 2,
    DROP_LANGUAGE = 3,
    DROP_THEME = 4,
    DROP_METRO_COLOR = 5,
    DROP_WIN95_COLOR = 6,
    DROP_WALLPAPER_MODE = 7,
    DROP_INPUTM_HOTKEY = 8,
    DROP_INPUTM_STARTUP = 9,
};

static uint32_t pixels[SETTINGS_W * SETTINGS_H];
static struct reliefos_display_state display_state;
static struct reliefos_fb_capabilities framebuffer_caps;
static struct reliefos_appearance_state appearance_state;
static struct reliefos_user_info current_user;
static struct reliefos_user_info *users;
static uint32_t user_scroll;
static uint32_t user_count;
static uint32_t selected_user;
static uint8_t active_page;
static uint8_t active_drop;
static struct reliefos_ui_tab_state settings_tabs;
static struct settings_sound_model sound_model;
static char status_text[160] = "Ready";
static char ntp_runtime_state[16] = "unknown";
static char ntp_runtime_detail[96] = "runtime state unavailable";

struct settings_inputm_entry {
    char id[TEXT_INPUT_ID_LEN];
    char path[RELIEFOS_FS_PATH_LEN];
    char settings_path[RELIEFOS_FS_PATH_LEN];
    char settings_app[RELIEFOS_FS_PATH_LEN];
    uint32_t config_index;
    uint32_t startup_mode;
    uint32_t order;
    uint8_t enabled;
};

struct settings_inputm_option {
    char key[SETTINGS_INPUTM_OPTION_KEY_LEN];
    char label[SETTINGS_INPUTM_OPTION_LABEL_LEN];
    uint8_t value;
};

static struct settings_inputm_entry inputm_entries[SETTINGS_INPUTM_ROWS];
static uint32_t inputm_entry_count;
static uint32_t inputm_selected;
static char inputm_default[TEXT_INPUT_ID_LEN] = "en";
static char inputm_hotkey[16] = "win-space";
static struct settings_inputm_option inputm_options[SETTINGS_INPUTM_OPTION_COUNT];
static uint32_t inputm_option_count;

struct assoc_row {
    const char *extension;
    const char *description;
};

struct service_row {
    const char *key;
    const char *name;
    const char *detail;
    uint8_t enabled;
    uint8_t locked;
};

static const char *mode_labels[SETTINGS_MODE_COUNT] = {
    "1920 x 1080",
    "1600 x 900",
    "1280 x 800",
    "1280 x 720",
    "1024 x 768",
};

static const uint32_t mode_widths[SETTINGS_MODE_COUNT] = {1920, 1600, 1280, 1280, 1024};
static const uint32_t mode_heights[SETTINGS_MODE_COUNT] = {1080, 900, 800, 720, 768};
static const uint32_t scale_values[SETTINGS_SCALE_COUNT] = {1, 2, 3};
static const char *scale_labels[SETTINGS_SCALE_COUNT] = {"1x", "2x", "3x"};
static const struct assoc_row assoc_rows[SETTINGS_ASSOC_ROWS] = {
    {".txt", N_("Text documents")},
    {".md", N_("Markdown notes")},
    {".html", N_("HTML pages")},
    {".htm", N_("HTML pages")},
    {".bmp", N_("Bitmap images")},
    {".hlp", N_("Help files")},
};
/* Open-with buttons are populated from the application registry.  The UI has
 * four compact slots, while the registry may contain more handlers. */
#define SETTINGS_ASSOC_CANDIDATE_MAX 4U
static struct reliefos_app_info settings_assoc_apps[RELIEFOS_APP_REGISTRY_MAX];
static uint32_t settings_assoc_app_count;
static uint8_t settings_assoc_apps_loaded;
static const uint32_t settings_assoc_button_x[SETTINGS_ASSOC_CANDIDATE_MAX] = {
    450U, 520U, 596U, 660U,
};
static const uint32_t settings_assoc_button_w[SETTINGS_ASSOC_CANDIDATE_MAX] = {
    64U, 70U, 58U, 50U,
};
static struct service_row service_rows[SETTINGS_SERVICE_ROWS] = {
    {"desktop", N_("Desktop window service"), N_("Required system shell. Cannot be disabled here."), 1, 1},
    {"dhcp", N_("DHCP auto connect"), N_("Boot and service runtime should request an address automatically."), 1, 0},
    {"network_icon", N_("Taskbar network icon"), N_("Show network state in the desktop taskbar."), 1, 0},
    {"rtc_clock", N_("RTC taskbar clock"), N_("Show the hardware clock in the taskbar."), 1, 0},
    {"ntp_sync", N_("NTP time sync"), N_("Synchronize the system clock through pool.ntp.org."), 0, 0},
};

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (*pos + 1 < cap) {
        buf[(*pos)++] = ch;
        buf[*pos] = 0;
    }
}

static void append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text)
{
    while (text && *text) {
        append_char(buf, pos, cap, *text++);
    }
}

static void append_dec(char *buf, uint32_t *pos, uint32_t cap, uint32_t value)
{
    char tmp[16];
    uint32_t n = 0;
    if (value == 0) {
        append_char(buf, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        append_char(buf, pos, cap, tmp[--n]);
    }
}

static int hit_rect_i(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int text_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    if (!a || !b) {
        return 0;
    }
    while (a[i] && b[i] && a[i] == b[i]) {
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

static int text_eq_ignore_case(const char *a, const char *b)
{
    uint32_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        if (left != right) return 0;
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

static uint32_t text_len(const char *text)
{
    uint32_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n;
}

static uint16_t settings_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t settings_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t settings_read_le32s(const uint8_t *p)
{
    return (int32_t)settings_read_le32(p);
}

static const char *theme_color_label(uint32_t scheme)
{
    switch (scheme) {
    case RELIEFOS_UI_COLOR_SCHEME_TEAL:
        return T("Teal");
    case RELIEFOS_UI_COLOR_SCHEME_GREEN:
        return T("Green");
    case RELIEFOS_UI_COLOR_SCHEME_PURPLE:
        return T("Purple");
    case RELIEFOS_UI_COLOR_SCHEME_RED:
        return T("Red");
    case RELIEFOS_UI_COLOR_SCHEME_GRAPHITE:
        return T("Graphite");
    case RELIEFOS_UI_COLOR_SCHEME_PINK:
        return T("Kawaii Pink");
    default:
        return T("Blue");
    }
}

static const char *wallpaper_mode_label(uint32_t mode)
{
    switch (mode) {
    case RELIEFOS_WALLPAPER_MODE_FIT:
        return T("Fit");
    case RELIEFOS_WALLPAPER_MODE_CENTER:
        return T("Center");
    case RELIEFOS_WALLPAPER_MODE_TILE:
        return T("Tile");
    case RELIEFOS_WALLPAPER_MODE_STRETCH:
        return T("Stretch");
    default:
        return T("Fill");
    }
}

static void fill_theme_color_items(struct reliefos_ui_dropdown_item *items)
{
    if (!items) {
        return;
    }
    for (uint32_t i = 0; i < RELIEFOS_UI_COLOR_SCHEME_COUNT; ++i) {
        items[i].label = theme_color_label(i);
        items[i].id = i;
        items[i].flags = 0;
    }
}

static void fill_wallpaper_mode_items(struct reliefos_ui_dropdown_item *items)
{
    if (!items) {
        return;
    }
    for (uint32_t i = 0; i < RELIEFOS_WALLPAPER_MODE_COUNT; ++i) {
        items[i].label = wallpaper_mode_label(i);
        items[i].id = i;
        items[i].flags = 0;
    }
}

static int validate_wallpaper_bmp(const char *path)
{
    uint8_t header[54];
    struct reliefos_stat st;
    int fd;
    long got;
    uint32_t pixel_offset;
    uint32_t dib_size;
    int32_t width;
    int32_t height_signed;
    uint32_t height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t row_stride;
    if (!path || !path[0]) {
        return 0;
    }
    if (reliefos_stat_legacy(path, &st) < 0 || st.type != RELIEFOS_FS_TYPE_FILE ||
        st.size < sizeof(header) || st.size > SETTINGS_WALLPAPER_BMP_MAX_BYTES) {
        return 0;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    got = read(fd, header, sizeof(header));
    close(fd);
    if (got < (long)sizeof(header) || header[0] != 'B' || header[1] != 'M') {
        return 0;
    }
    pixel_offset = settings_read_le32(header + 10);
    dib_size = settings_read_le32(header + 14);
    width = settings_read_le32s(header + 18);
    height_signed = settings_read_le32s(header + 22);
    planes = settings_read_le16(header + 26);
    bpp = settings_read_le16(header + 28);
    compression = settings_read_le32(header + 30);
    if (dib_size < 40 || width <= 0 || height_signed == 0 ||
        planes != 1 || (bpp != 24 && bpp != 32) || compression != 0) {
        return 0;
    }
    height = height_signed < 0
                 ? (uint32_t)(0u - (uint32_t)height_signed)
                 : (uint32_t)height_signed;
    if ((uint32_t)width > SETTINGS_WALLPAPER_MAX_W ||
        height > SETTINGS_WALLPAPER_MAX_H) {
        return 0;
    }
    row_stride = ((((uint32_t)width * bpp) + 31u) / 32u) * 4u;
    if ((uint64_t)pixel_offset + (uint64_t)row_stride * height > st.size) {
        return 0;
    }
    return 1;
}

static const char *role_label(uint32_t role)
{
    return role == RELIEFOS_AUTH_ROLE_ADMIN ? T("Administrator") : T("User");
}

static const char *program_label(const char *program)
{
    static char label[RELIEFOS_APP_NAME_LEN];
    if (program && program[0] &&
        reliefos_app_registry_label(program, label, sizeof(label)) == 0) {
        return label;
    }
    return program && program[0] ? program : T("None");
}

static void settings_load_assoc_apps(void)
{
    uint32_t total;
    settings_assoc_app_count = 0;
    settings_assoc_apps_loaded = 0;
    (void)reliefos_app_registry_refresh();
    total = reliefos_app_registry_count();
    for (uint32_t i = 0; i < total && settings_assoc_app_count <
                            RELIEFOS_APP_REGISTRY_MAX; ++i) {
        struct reliefos_app_info info;
        if (reliefos_app_registry_get(i, &info) < 0 ||
            (info.flags & RELIEFOS_APP_FLAG_OPEN_WITH) == 0 ||
            !info.extensions[0]) {
            continue;
        }
        settings_assoc_apps[settings_assoc_app_count++] = info;
    }
    settings_assoc_apps_loaded = 1;
}

static void settings_ensure_assoc_apps(void)
{
    if (!settings_assoc_apps_loaded) {
        settings_load_assoc_apps();
    }
}

static int settings_assoc_extension_matches(const char *list, const char *wanted)
{
    char item[32];
    uint32_t pos = 0;
    uint32_t out = 0;
    if (!list || !wanted || !wanted[0]) return 0;
    while (1) {
        char ch = list[pos++];
        if (ch == ',' || ch == 0) {
            while (out && (item[out - 1U] == ' ' || item[out - 1U] == '\t')) --out;
            item[out] = 0;
            if (text_eq_ignore_case(item, wanted)) return 1;
            out = 0;
            if (ch == 0) break;
            continue;
        }
        if (out + 1U < sizeof(item) && ch != ' ' && ch != '\t') item[out++] = ch;
    }
    return 0;
}

static uint32_t settings_assoc_row_candidates(uint32_t row,
                                              uint32_t *indices,
                                              uint32_t capacity)
{
    uint32_t count = 0;
    if (row >= SETTINGS_ASSOC_ROWS || !indices || capacity == 0) return 0;
    for (uint32_t i = 0; i < settings_assoc_app_count && count < capacity; ++i) {
        if (settings_assoc_extension_matches(settings_assoc_apps[i].extensions,
                                             assoc_rows[row].extension)) {
            indices[count++] = i;
        }
    }
    return count;
}

static const char *license_status_label(uint32_t status)
{
    switch (status) {
    case RELIEFOS_LICENSE_STATUS_OK:
        return T("Activated");
    case RELIEFOS_LICENSE_STATUS_MISSING:
        return T("Not activated");
    case RELIEFOS_LICENSE_STATUS_INVALID:
        return T("Invalid activation");
    case RELIEFOS_LICENSE_STATUS_NETWORK:
        return T("Network failure");
    case RELIEFOS_LICENSE_STATUS_CLOCK:
        return T("Clock failure");
    case RELIEFOS_LICENSE_STATUS_DENIED:
        return T("Activation denied");
    default:
        return T("Unknown");
    }
}

static const char *license_mode_label(const char *mode)
{
    if (text_eq(mode, "online")) {
        return T("Online");
    }
    if (text_eq(mode, "offline")) {
        return T("Offline");
    }
    return T("None");
}

static const char *value_or_dash(const char *value)
{
    return value && value[0] ? value : "-";
}

static void draw_field(struct reliefos_ui_surface *ui, int32_t y,
                       const char *label, const char *value)
{
    reliefos_ui_text(ui, 50, y, label, RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 176, y, SETTINGS_W - 226,
                           value_or_dash(value), RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
}

static void fake_path_for_extension(char *dst, uint32_t cap, const char *ext)
{
    uint32_t pos = 0;
    dst[0] = 0;
    append_text(dst, &pos, cap, "/sample");
    append_text(dst, &pos, cap, ext);
}

static int service_line_matches(const char *line, uint32_t len,
                                const char *key, uint8_t *value)
{
    uint32_t key_len = text_len(key);
    uint32_t pos = 0;
    if (!line || !key || !value || len <= key_len || line[key_len] != '=') {
        return 0;
    }
    while (pos < key_len) {
        if (line[pos] != key[pos]) {
            return 0;
        }
        ++pos;
    }
    *value = line[key_len + 1U] == '1' ||
             line[key_len + 1U] == 'y' ||
             line[key_len + 1U] == 'Y';
    return 1;
}

static void load_services_config(void)
{
    char cfg[SETTINGS_SERVICES_CONFIG_MAX];
    uint32_t len = 0;
    uint32_t pos = 0;
    service_rows[1].enabled = reliefos_openrc_enabled("reliefos-dhcp") == 1;
    service_rows[4].enabled = reliefos_openrc_enabled("reliefos-ntp") == 1;
    int fd = open(SETTINGS_SERVICES_PATH, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    while (len + 1U < sizeof(cfg)) {
        long got = read(fd, cfg + len, sizeof(cfg) - len - 1U);
        if (got < 0) {
            close(fd);
            return;
        }
        if (got == 0) {
            break;
        }
        len += (uint32_t)got;
    }
    close(fd);
    cfg[len] = 0;
    while (pos < len) {
        uint32_t start = pos;
        uint32_t line_len;
        while (pos < len && cfg[pos] != '\n' && cfg[pos] != '\r') {
            ++pos;
        }
        line_len = pos - start;
        while (pos < len && (cfg[pos] == '\n' || cfg[pos] == '\r')) {
            ++pos;
        }
        for (uint32_t i = 0; i < SETTINGS_SERVICE_ROWS; ++i) {
            uint8_t value = 0;
            if ((i == 2 || i == 3) &&
                service_line_matches(cfg + start, line_len,
                                     service_rows[i].key, &value)) {
                service_rows[i].enabled = value;
            }
        }
    }
}

static int settings_rc_child, settings_rc_kind, settings_rc_save;
static uint8_t settings_rc_dhcp, settings_rc_ntp;

static void refresh_ntp_runtime_state(void)
{
    if (settings_rc_child || settings_rc_save) return;
    settings_rc_kind = 1;
    settings_rc_child = reliefos_openrc_spawn("reliefos-ntp", "status");
    if (settings_rc_child < 0) {
        settings_rc_child = 0;
        copy_text(ntp_runtime_state, sizeof(ntp_runtime_state), "failed");
        copy_text(ntp_runtime_detail, sizeof(ntp_runtime_detail), "OpenRC worker failed");
    }
}

static void save_services_config(void)
{
    if (current_user.role != RELIEFOS_AUTH_ROLE_ADMIN) {
        copy_text(status_text, sizeof(status_text), T("Administrator rights required"));
        return;
    }
    if (settings_rc_save || settings_rc_kind > 1) return;
    settings_rc_dhcp = service_rows[1].enabled;
    settings_rc_ntp = service_rows[4].enabled;
    settings_rc_save = 1;
    copy_text(status_text, sizeof(status_text), T("Updating OpenRC..."));
}

static void write_services_preferences(void)
{
    char cfg[SETTINGS_SERVICES_CONFIG_MAX];
    uint32_t pos = 0;
    int fd;
    if (current_user.role != RELIEFOS_AUTH_ROLE_ADMIN) {
        copy_text(status_text, sizeof(status_text),
                  T("Administrator rights required"));
        return;
    }
    cfg[0] = 0;
    append_text(cfg, &pos, sizeof(cfg), "# ReliefOS taskbar preferences\n");
    for (uint32_t i = 2; i <= 3; ++i) {
        append_text(cfg, &pos, sizeof(cfg), service_rows[i].key);
        append_char(cfg, &pos, sizeof(cfg), '=');
        append_char(cfg, &pos, sizeof(cfg), service_rows[i].enabled ? '1' : '0');
        append_char(cfg, &pos, sizeof(cfg), '\n');
    }
    fd = open(SETTINGS_SERVICES_PATH,
              RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_TRUNC, 0644);
    if (fd < 0) {
        copy_text(status_text, sizeof(status_text),
                  T("Could not save services."));
        return;
    }
    if (write(fd, cfg, pos) != (long)pos) {
        copy_text(status_text, sizeof(status_text),
                  T("Could not save services."));
    } else {
        copy_text(status_text, sizeof(status_text),
                  T("Service settings saved"));
    }
    close(fd);
}

static int poll_settings_openrc(void)
{
    int changed = 0;
    if (settings_rc_child) {
        int result = 125, ready = reliefos_openrc_poll(settings_rc_child, &result);
        if (!ready) return 0;
        settings_rc_child = 0;
        if (ready < 0) result = 125;
        changed = 1;
        if (settings_rc_kind == 1) {
            copy_text(ntp_runtime_state, sizeof(ntp_runtime_state), result == 0 ? "running" : result == 3 ? "stopped" : "failed");
            snprintf(ntp_runtime_detail, sizeof(ntp_runtime_detail), "OpenRC exit=%d (daemon status, not sync proof)", result);
            settings_rc_kind = 0;
        } else if (result) {
            snprintf(status_text, sizeof(status_text), "OpenRC update failed: exit=%d", result);
            settings_rc_kind = settings_rc_save = 0;
            load_services_config();
        } else if (settings_rc_kind == 2) {
            settings_rc_kind = 3;
            settings_rc_child = reliefos_openrc_spawn("reliefos-ntp", settings_rc_ntp ? "enable" : "disable");
        } else {
            settings_rc_kind = settings_rc_save = 0;
            write_services_preferences();
        }
    }
    if (!settings_rc_child && settings_rc_save && settings_rc_kind == 0) {
        settings_rc_kind = 2;
        settings_rc_child = reliefos_openrc_spawn("reliefos-dhcp", settings_rc_dhcp ? "enable" : "disable");
    }
    if (settings_rc_child < 0) {
        settings_rc_child = settings_rc_kind = settings_rc_save = 0;
        copy_text(status_text, sizeof(status_text), "OpenRC worker failed");
        changed = 1;
    }
    return changed;
}

static int mode_supported(uint32_t mode, uint32_t scale_index)
{
    uint32_t scale;
    uint64_t physical_width;
    uint64_t physical_height;
    uint64_t required_bytes;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_bytes;
    if (mode >= SETTINGS_MODE_COUNT || scale_index >= SETTINGS_SCALE_COUNT) {
        return 0;
    }
    scale = scale_values[scale_index];
    physical_width = (uint64_t)mode_widths[mode] * scale;
    physical_height = (uint64_t)mode_heights[mode] * scale;
    required_bytes = physical_width * physical_height * sizeof(uint32_t);
    max_width = framebuffer_caps.max_width ? framebuffer_caps.max_width : display_state.fb_width;
    max_height = framebuffer_caps.max_height ? framebuffer_caps.max_height : display_state.fb_height;
    max_bytes = framebuffer_caps.max_bytes
                    ? framebuffer_caps.max_bytes
                    : display_state.fb_width * display_state.fb_height * sizeof(uint32_t);
    return physical_width <= max_width && physical_height <= max_height &&
           required_bytes <= max_bytes;
}

static void refresh_display_state(void)
{
    int state_available = reliefos_display_get_state(&display_state) > 0;
    if (reliefos_fb_capabilities(&framebuffer_caps) < 0) {
        framebuffer_caps.bytes_per_pixel = 4;
        framebuffer_caps.capabilities = 0;
        framebuffer_caps.max_width = state_available ? display_state.fb_width : 1920;
        framebuffer_caps.max_height = state_available ? display_state.fb_height : 1080;
        framebuffer_caps.max_bytes = framebuffer_caps.max_width * framebuffer_caps.max_height *
                                    sizeof(uint32_t);
        framebuffer_caps.backend = RELIEFOS_FB_BACKEND_BOOT;
    }
    if (!state_available) {
        display_state.fb_width = 1920;
        display_state.fb_height = 1080;
        display_state.logical_width = 1280;
        display_state.logical_height = 800;
        display_state.scale = 1;
        display_state.mode_index = 2;
        display_state.scale_index = 0;
        display_state.pending_confirm = 0;
        display_state.confirm_remaining_ms = 0;
    }
}

static void refresh_appearance_state(void)
{
    if (reliefos_appearance_get_state(&appearance_state) <= 0) {
        appearance_state.theme = reliefos_ui_theme();
        appearance_state.metro_color_scheme =
            reliefos_ui_theme_color_scheme(RELIEFOS_UI_THEME_METRO);
        appearance_state.win95_color_scheme =
            reliefos_ui_theme_color_scheme(RELIEFOS_UI_THEME_WIN95);
        appearance_state.wallpaper_mode = RELIEFOS_WALLPAPER_MODE_FILL;
        copy_text(appearance_state.wallpaper_path,
                  sizeof(appearance_state.wallpaper_path),
                  SETTINGS_DEFAULT_WALLPAPER_PATH);
    }
}

static void refresh_users(void)
{
    uint32_t count = 0;
    current_user = (struct reliefos_user_info){0};
    (void)reliefos_auth_current(&current_user);
    if (current_user.role == RELIEFOS_AUTH_ROLE_ADMIN) {
        (void)reliefos_auth_users_alloc(&users, 1, &count);
    } else if (current_user.username[0]) {
        if (!users) users = calloc(1, sizeof(*users));
        if (!users) { user_count = 0; return; }
        users[0] = current_user;
        count = 1;
    }
    user_count = count;
    if (selected_user >= user_count) {
        selected_user = user_count ? user_count - 1 : 0;
    }
}

static int inputm_config_path(char *path, uint32_t capacity)
{
    uint32_t home_len;
    const char *name = ".inputm.conf";
    if (!path || !capacity || !current_user.username[0] || !current_user.home[0]) {
        return 0;
    }
    home_len = text_len(current_user.home);
    if (home_len + 1U + text_len(name) >= capacity) {
        return 0;
    }
    copy_text(path, capacity, current_user.home);
    path[home_len] = '/';
    copy_text(path + home_len + 1U, capacity - home_len - 1U, name);
    return 1;
}

static int inputm_config_get(const char *config, const char *key,
                             char *value, uint32_t capacity)
{
    uint32_t key_len = text_len(key);
    uint32_t pos = 0;
    uint8_t found = 0;
    if (!config || !key || !key_len || !value || !capacity) {
        return 0;
    }
    value[0] = 0;
    while (config[pos]) {
        uint32_t start = pos;
        uint32_t end;
        uint32_t out = 0;
        uint8_t match = 1;
        while (config[pos] && config[pos] != '\n' && config[pos] != '\r') {
            ++pos;
        }
        end = pos;
        while (config[pos] == '\n' || config[pos] == '\r') {
            ++pos;
        }
        if (end <= start + key_len || config[start + key_len] != '=') {
            continue;
        }
        for (uint32_t i = 0; i < key_len; ++i) {
            if (config[start + i] != key[i]) {
                match = 0;
                break;
            }
        }
        if (!match) {
            continue;
        }
        start += key_len + 1U;
        while (start < end && out + 1U < capacity) {
            value[out++] = config[start++];
        }
        value[out] = 0;
        found = 1;
    }
    return found;
}

static uint32_t inputm_parse_u32(const char *text, uint32_t fallback)
{
    uint32_t value = 0;
    uint32_t digits = 0;
    while (text && *text >= '0' && *text <= '9') {
        value = value * 10U + (uint32_t)(*text - '0');
        ++digits;
        ++text;
    }
    return digits ? value : fallback;
}

static void inputm_provider_key(char *key, uint32_t capacity, uint32_t index,
                                const char *field)
{
    uint32_t pos = 0;
    key[0] = 0;
    append_text(key, &pos, capacity, "provider");
    append_dec(key, &pos, capacity, index);
    append_char(key, &pos, capacity, '_');
    append_text(key, &pos, capacity, field);
}

static int inputm_append_config(const char *key, const char *value)
{
    char path[RELIEFOS_FS_PATH_LEN];
    char line[RELIEFOS_FS_PATH_LEN + 80U];
    uint32_t pos = 0;
    int fd;
    long wrote;
    if (!key || !value || !inputm_config_path(path, sizeof(path))) {
        return 0;
    }
    line[0] = 0;
    append_char(line, &pos, sizeof(line), '\n');
    append_text(line, &pos, sizeof(line), key);
    append_char(line, &pos, sizeof(line), '=');
    append_text(line, &pos, sizeof(line), value);
    append_char(line, &pos, sizeof(line), '\n');
    fd = open(path, RELIEFOS_O_WRONLY | RELIEFOS_O_CREAT | RELIEFOS_O_APPEND, 0666);
    if (fd < 0) {
        return 0;
    }
    wrote = write(fd, line, pos);
    close(fd);
    if (wrote == (long)pos) {
        (void)text_input_notify_config(current_user.uid);
        return 1;
    }
    return 0;
}

static int inputm_key_is_safe(const char *key)
{
    uint32_t i = 0;
    if (!key || !key[0]) {
        return 0;
    }
    while (key[i]) {
        char ch = key[i++];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return 0;
        }
    }
    return i < SETTINGS_INPUTM_OPTION_KEY_LEN;
}

static int inputm_schema_value(const char *line, uint32_t length,
                               const char *field, char *out, uint32_t capacity)
{
    uint32_t field_len = text_len(field);
    uint32_t pos = 0;
    if (!line || !field || !out || capacity == 0 || length <= field_len ||
        line[field_len] != '=') {
        return 0;
    }
    for (uint32_t i = 0; i < field_len; ++i) {
        if (line[i] != field[i]) {
            return 0;
        }
    }
    while (field_len + 1U + pos < length && pos + 1U < capacity) {
        out[pos] = line[field_len + 1U + pos];
        ++pos;
    }
    out[pos] = 0;
    return 1;
}

static void inputm_add_schema_option(const char *config, const char *key,
                                     const char *type, const char *default_value,
                                     const char *label)
{
    struct settings_inputm_option *option;
    char value[16];
    if (inputm_option_count >= SETTINGS_INPUTM_OPTION_COUNT ||
        !inputm_key_is_safe(key) || !text_eq(type, "bool")) {
        return;
    }
    option = &inputm_options[inputm_option_count];
    *option = (struct settings_inputm_option){0};
    copy_text(option->key, sizeof(option->key), key);
    copy_text(option->label, sizeof(option->label), label && label[0] ? label : key);
    if (inputm_config_get(config, key, value, sizeof(value))) {
        option->value = inputm_parse_u32(value, 0) ? 1 : 0;
    } else {
        option->value = inputm_parse_u32(default_value, 0) ? 1 : 0;
    }
    ++inputm_option_count;
}

static void inputm_load_extension_options(const char *config)
{
    struct settings_inputm_entry *entry;
    char schema[SETTINGS_INPUTM_CONFIG_MAX];
    char key[SETTINGS_INPUTM_OPTION_KEY_LEN] = {0};
    char type[16] = {0};
    char default_value[16] = {0};
    char label[SETTINGS_INPUTM_OPTION_LABEL_LEN] = {0};
    uint8_t in_setting = 0;
    uint32_t pos = 0;
    int fd;
    long got;

    inputm_option_count = 0;
    entry = inputm_selected < inputm_entry_count ? &inputm_entries[inputm_selected] : 0;
    if (!entry || !entry->settings_path[0]) {
        return;
    }
    fd = open(entry->settings_path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    got = read(fd, schema, sizeof(schema) - 1U);
    close(fd);
    if (got <= 0) {
        return;
    }
    schema[got] = 0;
    while (schema[pos]) {
        uint32_t start = pos;
        uint32_t end;
        while (schema[pos] && schema[pos] != '\n' && schema[pos] != '\r') {
            ++pos;
        }
        end = pos;
        while (schema[pos] == '\n' || schema[pos] == '\r') {
            ++pos;
        }
        if (end - start == 9U &&
            schema[start] == '[' && schema[start + 1U] == 's' &&
            schema[start + 2U] == 'e' && schema[start + 3U] == 't' &&
            schema[start + 4U] == 't' && schema[start + 5U] == 'i' &&
            schema[start + 6U] == 'n' && schema[start + 7U] == 'g' &&
            schema[start + 8U] == ']') {
            if (in_setting) {
                inputm_add_schema_option(config, key, type, default_value, label);
            }
            key[0] = 0;
            type[0] = 0;
            default_value[0] = 0;
            label[0] = 0;
            in_setting = 1;
            continue;
        }
        if (!in_setting) {
            continue;
        }
        if (inputm_schema_value(schema + start, end - start, "key", key, sizeof(key)) ||
            inputm_schema_value(schema + start, end - start, "type", type, sizeof(type)) ||
            inputm_schema_value(schema + start, end - start, "default", default_value,
                                sizeof(default_value)) ||
            inputm_schema_value(schema + start, end - start, "label", label, sizeof(label))) {
            continue;
        }
    }
    if (in_setting) {
        inputm_add_schema_option(config, key, type, default_value, label);
    }
}

static void inputm_sort_entries(void)
{
    for (uint32_t i = 1; i < inputm_entry_count; ++i) {
        for (uint32_t j = i + 1U; j < inputm_entry_count; ++j) {
            if (inputm_entries[j].order < inputm_entries[i].order) {
                struct settings_inputm_entry temp = inputm_entries[i];
                inputm_entries[i] = inputm_entries[j];
                inputm_entries[j] = temp;
            }
        }
    }
}

static void inputm_reload_extension_options(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    char config[SETTINGS_INPUTM_CONFIG_MAX];
    int fd;
    long got;
    inputm_option_count = 0;
    if (!inputm_config_path(path, sizeof(path))) {
        return;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    got = read(fd, config, sizeof(config) - 1U);
    close(fd);
    if (got <= 0) {
        return;
    }
    config[got] = 0;
    inputm_load_extension_options(config);
}

static void inputm_load_settings(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    char config[SETTINGS_INPUTM_CONFIG_MAX];
    char value[RELIEFOS_FS_PATH_LEN];
    int fd;
    long got;
    uint32_t configured = 0;
    inputm_entries[0] = (struct settings_inputm_entry){0};
    copy_text(inputm_entries[0].id, sizeof(inputm_entries[0].id), "en");
    inputm_entries[0].enabled = 1;
    inputm_entries[0].startup_mode = TEXT_INPUT_START_MANUAL;
    inputm_entry_count = 1;
    inputm_selected = 0;
    copy_text(inputm_default, sizeof(inputm_default), "en");
    copy_text(inputm_hotkey, sizeof(inputm_hotkey), "win-space");
    inputm_option_count = 0;
    if (!inputm_config_path(path, sizeof(path))) {
        return;
    }
    fd = open(path, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    got = read(fd, config, sizeof(config) - 1U);
    close(fd);
    if (got <= 0) {
        return;
    }
    config[got] = 0;
    if (inputm_config_get(config, "default", value, sizeof(value)) && value[0]) {
        copy_text(inputm_default, sizeof(inputm_default), value);
    }
    if (inputm_config_get(config, "inputm_hotkey", value, sizeof(value)) && value[0]) {
        copy_text(inputm_hotkey, sizeof(inputm_hotkey), value);
    }
    if (inputm_config_get(config, "provider_count", value, sizeof(value))) {
        configured = inputm_parse_u32(value, 0);
    }
    if (configured > TEXT_INPUT_MAX_PROVIDERS) {
        configured = TEXT_INPUT_MAX_PROVIDERS;
    }
    for (uint32_t i = 0; i < configured && inputm_entry_count < SETTINGS_INPUTM_ROWS; ++i) {
        char key[48];
        struct settings_inputm_entry *entry = &inputm_entries[inputm_entry_count];
        inputm_provider_key(key, sizeof(key), i, "id");
        if (!inputm_config_get(config, key, value, sizeof(value)) || !value[0] ||
            text_eq(value, "en")) {
            continue;
        }
        *entry = (struct settings_inputm_entry){0};
        copy_text(entry->id, sizeof(entry->id), value);
        entry->config_index = i;
        entry->enabled = 1;
        entry->startup_mode = TEXT_INPUT_START_ON_DEMAND;
        entry->order = i + 1U;
        inputm_provider_key(key, sizeof(key), i, "path");
        if (inputm_config_get(config, key, value, sizeof(value))) {
            copy_text(entry->path, sizeof(entry->path), value);
        }
        inputm_provider_key(key, sizeof(key), i, "settings");
        if (inputm_config_get(config, key, value, sizeof(value))) {
            copy_text(entry->settings_path, sizeof(entry->settings_path), value);
        }
        inputm_provider_key(key, sizeof(key), i, "settings_app");
        if (inputm_config_get(config, key, value, sizeof(value))) {
            copy_text(entry->settings_app, sizeof(entry->settings_app), value);
        }
        inputm_provider_key(key, sizeof(key), i, "enabled");
        if (inputm_config_get(config, key, value, sizeof(value))) {
            entry->enabled = inputm_parse_u32(value, 1) ? 1 : 0;
        }
        inputm_provider_key(key, sizeof(key), i, "startup");
        if (inputm_config_get(config, key, value, sizeof(value))) {
            uint32_t mode = inputm_parse_u32(value, TEXT_INPUT_START_ON_DEMAND);
            entry->startup_mode = mode <= TEXT_INPUT_START_ON_DEMAND ? mode :
                                  TEXT_INPUT_START_ON_DEMAND;
        }
        inputm_provider_key(key, sizeof(key), i, "order");
        if (inputm_config_get(config, key, value, sizeof(value))) {
            uint32_t order = inputm_parse_u32(value, i + 1U);
            entry->order = order ? order : i + 1U;
        }
        ++inputm_entry_count;
    }
    inputm_sort_entries();
    for (uint32_t i = 1; i < inputm_entry_count; ++i) {
        if (text_eq(inputm_entries[i].id, inputm_default)) {
            inputm_selected = i;
            break;
        }
    }
    inputm_load_extension_options(config);
}

static const char *inputm_startup_label(uint32_t mode)
{
    if (mode == TEXT_INPUT_START_LOGIN) {
        return T("At sign-in");
    }
    if (mode == TEXT_INPUT_START_ON_DEMAND) {
        return T("On demand");
    }
    return T("Manual");
}

static void request_display(uint32_t action, uint32_t mode, uint32_t scale)
{
    struct reliefos_display_request request;
    request.action = action;
    request.mode_index = mode;
    request.scale_index = scale;
    (void)reliefos_display_request(&request);
}

static void request_appearance_change(const char *ok_text)
{
    struct reliefos_appearance_request request;
    request.theme = appearance_state.theme;
    request.metro_color_scheme = appearance_state.metro_color_scheme;
    request.win95_color_scheme = appearance_state.win95_color_scheme;
    request.wallpaper_mode = appearance_state.wallpaper_mode;
    copy_text(request.wallpaper_path, sizeof(request.wallpaper_path),
              appearance_state.wallpaper_path);
    if (reliefos_appearance_request_theme(&request) > 0) {
        (void)reliefos_ui_theme_set_appearance(request.theme,
                                             request.metro_color_scheme,
                                             request.win95_color_scheme);
        copy_text(status_text, sizeof(status_text),
                  ok_text ? ok_text : T("Personalization updated"));
    } else {
        copy_text(status_text, sizeof(status_text),
                  T("Could not apply personalization"));
    }
}

static const char *mode_label(void)
{
    return display_state.mode_index < SETTINGS_MODE_COUNT
               ? mode_labels[display_state.mode_index]
               : mode_labels[0];
}

static const char *scale_label(void)
{
    return display_state.scale_index < SETTINGS_SCALE_COUNT
               ? scale_labels[display_state.scale_index]
               : scale_labels[0];
}

static unsigned selected_language;

static const char *language_label(void)
{
    return language_options[selected_language].name;
}

static const char *theme_label(void)
{
    return appearance_state.theme == RELIEFOS_UI_THEME_WIN95 ? "Win95" : "Metro";
}

static void draw_display_page(struct reliefos_ui_surface *ui)
{
    char line[128];
    uint32_t pos = 0;
    struct reliefos_ui_dropdown_item mode_items[SETTINGS_MODE_COUNT];
    struct reliefos_ui_dropdown_item scale_items[SETTINGS_SCALE_COUNT];
    struct reliefos_ui_dropdown_item lang_items[2];
    for (uint32_t i = 0; i < SETTINGS_MODE_COUNT; ++i) {
        mode_items[i].label = mode_labels[i];
        mode_items[i].id = i;
        mode_items[i].flags = mode_supported(i, display_state.scale_index) ? 0 : RELIEFOS_UI_MENU_DISABLED;
    }
    for (uint32_t i = 0; i < SETTINGS_SCALE_COUNT; ++i) {
        scale_items[i].label = scale_labels[i];
        scale_items[i].id = i;
        scale_items[i].flags = mode_supported(display_state.mode_index, i) ? 0 : RELIEFOS_UI_MENU_DISABLED;
    }
    lang_items[0] = (struct reliefos_ui_dropdown_item){language_options[0].name, 0, 0};
    lang_items[1] = (struct reliefos_ui_dropdown_item){language_options[1].name, 1, 0};

    append_text(line, &pos, sizeof(line), T("Framebuffer "));
    append_dec(line, &pos, sizeof(line), display_state.fb_width);
    append_char(line, &pos, sizeof(line), 'x');
    append_dec(line, &pos, sizeof(line), display_state.fb_height);
    append_text(line, &pos, sizeof(line), "  ");
    append_text(line, &pos, sizeof(line), T("Desktop "));
    append_dec(line, &pos, sizeof(line), display_state.logical_width);
    append_char(line, &pos, sizeof(line), 'x');
    append_dec(line, &pos, sizeof(line), display_state.logical_height);
    reliefos_ui_text_clipped(ui, 34, 64, SETTINGS_W - 68, line, RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 44, 104, T("Resolution"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 98, 190, mode_label(), active_drop == DROP_RESOLUTION, 0);
    reliefos_ui_text(ui, 44, 144, T("Scale"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 138, 190, scale_label(), active_drop == DROP_SCALE, 0);
    reliefos_ui_slider(ui, 370, 138, 150, RELIEFOS_UI_BUTTON_H,
                     display_state.scale_index,
                     SETTINGS_SCALE_COUNT > 1 ? SETTINGS_SCALE_COUNT - 1 : 1,
                     0);
    reliefos_ui_text(ui, 44, 184, T("Language"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 178, 190, language_label(), active_drop == DROP_LANGUAGE, 0);
    if (display_state.pending_confirm) {
        uint32_t seconds = (display_state.confirm_remaining_ms + 999) / 1000;
        pos = 0;
        line[0] = 0;
        append_text(line, &pos, sizeof(line), T("Keep these display settings? Reverting in "));
        append_dec(line, &pos, sizeof(line), seconds);
        append_text(line, &pos, sizeof(line), T("s"));
        reliefos_ui_panel(ui, 44, 272, SETTINGS_W - 88, 64, RELIEFOS_UI_LIGHT);
        reliefos_ui_text_clipped(ui, 54, 284, SETTINGS_W - 108, line, RELIEFOS_UI_BLACK, RELIEFOS_UI_LIGHT);
        reliefos_ui_button(ui, 54, 306, 82, RELIEFOS_UI_BUTTON_H, T("Keep"), 0);
        reliefos_ui_button(ui, 146, 306, 82, RELIEFOS_UI_BUTTON_H, T("Revert"), 0);
    }
    if (active_drop == DROP_LANGUAGE) {
        reliefos_ui_dropdown(ui, 160, 202, 190, lang_items, 2,
                           selected_language, SETTINGS_DROPDOWN_ROW_H, 1000);
    } else if (active_drop == DROP_SCALE) {
        reliefos_ui_dropdown(ui, 160, 162, 190, scale_items, SETTINGS_SCALE_COUNT,
                           display_state.scale_index, SETTINGS_DROPDOWN_ROW_H, 1000);
    } else if (active_drop == DROP_RESOLUTION) {
        reliefos_ui_dropdown(ui, 160, 122, 190, mode_items, SETTINGS_MODE_COUNT,
                           display_state.mode_index, SETTINGS_DROPDOWN_ROW_H, 1000);
    }
}

static void draw_personalization_page(struct reliefos_ui_surface *ui)
{
    struct reliefos_ui_dropdown_item theme_items[2];
    struct reliefos_ui_dropdown_item metro_items[RELIEFOS_UI_COLOR_SCHEME_COUNT];
    struct reliefos_ui_dropdown_item win95_items[RELIEFOS_UI_COLOR_SCHEME_COUNT];
    struct reliefos_ui_dropdown_item wallpaper_items[RELIEFOS_WALLPAPER_MODE_COUNT];
    uint32_t disabled = current_user.username[0] ? 0 : RELIEFOS_UI_BUTTON_DISABLED;
    theme_items[0] = (struct reliefos_ui_dropdown_item){"Metro", RELIEFOS_UI_THEME_METRO, 0};
    theme_items[1] = (struct reliefos_ui_dropdown_item){"Win95", RELIEFOS_UI_THEME_WIN95, 0};
    fill_theme_color_items(metro_items);
    fill_theme_color_items(win95_items);
    fill_wallpaper_mode_items(wallpaper_items);

    reliefos_ui_text(ui, 34, 64,
                   T("Personalization is saved for the current user. Metro and Win95 keep separate colors."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);

    reliefos_ui_text(ui, 44, 104, T("Theme style"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 98, 190, theme_label(),
                       active_drop == DROP_THEME, disabled);

    reliefos_ui_text(ui, 44, 144, T("Metro color"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 138, 190,
                       theme_color_label(appearance_state.metro_color_scheme),
                       active_drop == DROP_METRO_COLOR, disabled);
    reliefos_ui_rect(ui, 364, 141, 28, 18,
                   reliefos_ui_theme_scheme_accent(RELIEFOS_UI_THEME_METRO,
                                                 appearance_state.metro_color_scheme));

    reliefos_ui_text(ui, 44, 184, T("Win95 color"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 178, 190,
                       theme_color_label(appearance_state.win95_color_scheme),
                       active_drop == DROP_WIN95_COLOR, disabled);
    reliefos_ui_rect(ui, 364, 181, 28, 18,
                   reliefos_ui_theme_scheme_accent(RELIEFOS_UI_THEME_WIN95,
                                                 appearance_state.win95_color_scheme));

    reliefos_ui_text(ui, 44, 224, T("Wallpaper"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_field(ui, 160, 218, 318, appearance_state.wallpaper_path,
                         RELIEFOS_UI_EDIT_READONLY | disabled);
    reliefos_ui_button(ui, 486, 218, 82, RELIEFOS_UI_BUTTON_H,
                     T("Browse"), disabled);
    reliefos_ui_button(ui, 576, 218, 82, RELIEFOS_UI_BUTTON_H,
                     T("Default"), disabled);

    reliefos_ui_text(ui, 44, 264, T("Display mode"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 160, 258, 190,
                       wallpaper_mode_label(appearance_state.wallpaper_mode),
                       active_drop == DROP_WALLPAPER_MODE, disabled);
    reliefos_ui_text(ui, 360, 264,
                   T("BMP only, up to 1280 x 720."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);

    if (!current_user.username[0]) {
        reliefos_ui_text(ui, 44, 318,
                       T("Sign in to change personalization."),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    }

    if (active_drop == DROP_THEME) {
        reliefos_ui_dropdown(ui, 160, 122, 190, theme_items, 2,
                           appearance_state.theme, SETTINGS_DROPDOWN_ROW_H, 1000);
    } else if (active_drop == DROP_METRO_COLOR) {
        reliefos_ui_dropdown(ui, 160, 162, 190, metro_items,
                           RELIEFOS_UI_COLOR_SCHEME_COUNT,
                           appearance_state.metro_color_scheme,
                           SETTINGS_DROPDOWN_ROW_H, 1000);
    } else if (active_drop == DROP_WIN95_COLOR) {
        reliefos_ui_dropdown(ui, 160, 202, 190, win95_items,
                           RELIEFOS_UI_COLOR_SCHEME_COUNT,
                           appearance_state.win95_color_scheme,
                           SETTINGS_DROPDOWN_ROW_H, 1000);
    } else if (active_drop == DROP_WALLPAPER_MODE) {
        reliefos_ui_dropdown(ui, 160, 282, 190, wallpaper_items,
                           RELIEFOS_WALLPAPER_MODE_COUNT,
                           appearance_state.wallpaper_mode,
                           SETTINGS_DROPDOWN_ROW_H, 1000);
    }
}

static void draw_input_methods_page(struct reliefos_ui_surface *ui)
{
    const struct reliefos_ui_list_column cols[] = {
        {T("Input method"), 220},
        {T("Enable"), 90},
        {T("Startup"), 150},
    };
    struct reliefos_ui_dropdown_item startup_items[3] = {
        {T("Manual"), TEXT_INPUT_START_MANUAL, 0},
        {T("At sign-in"), TEXT_INPUT_START_LOGIN, 0},
        {T("On demand"), TEXT_INPUT_START_ON_DEMAND, 0},
    };
    struct reliefos_ui_dropdown_item hotkey_items[2] = {
        {"Win + Space", 0, 0},
        {"Alt + Shift", 1, 0},
    };
    struct settings_inputm_entry *selected =
        inputm_selected < inputm_entry_count ? &inputm_entries[inputm_selected] : 0;
    reliefos_ui_text(ui, 34, 64,
                   T("Input methods and learning data are isolated for the current user."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_listview_header(ui, 34, 90, 460, cols, 3);
    for (uint32_t i = 0; i < inputm_entry_count && i < 5U; ++i) {
        const char *cells[3];
        cells[0] = inputm_entries[i].id;
        cells[1] = inputm_entries[i].enabled ? T("Yes") : T("No");
        cells[2] = inputm_startup_label(inputm_entries[i].startup_mode);
        reliefos_ui_listview_row(ui, 34, 118 + i * 27U, 460, cols, cells, 3,
                               i == inputm_selected ? RELIEFOS_UI_MENU_SELECTED : 0);
    }
    reliefos_ui_button(ui, 510, 92, 156, RELIEFOS_UI_BUTTON_H,
                     selected && selected->enabled ? T("Disable") :
                                                     T("Enable"),
                     !selected || !current_user.username[0] || inputm_selected == 0 ?
                         RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_button(ui, 510, 126, 156, RELIEFOS_UI_BUTTON_H,
                     T("Use as default"),
                     !selected || !selected->enabled || !current_user.username[0] ?
                         RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_combobox(ui, 510, 160, 156,
                        selected ? inputm_startup_label(selected->startup_mode) : "-",
                        active_drop == DROP_INPUTM_STARTUP,
                        !selected || !current_user.username[0] || inputm_selected == 0 ?
                            RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_button(ui, 510, 194, 74, RELIEFOS_UI_BUTTON_H,
                     T("Move up"),
                     !selected || !current_user.username[0] || inputm_selected <= 1U ?
                         RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_button(ui, 592, 194, 74, RELIEFOS_UI_BUTTON_H,
                     T("Move down"),
                     !selected || !current_user.username[0] || inputm_selected == 0 ||
                         inputm_selected + 1U >= inputm_entry_count ?
                         RELIEFOS_UI_BUTTON_DISABLED : 0);
    reliefos_ui_text(ui, 44, 266, T("Switch shortcut"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_combobox(ui, 170, 260, 180,
                        text_eq(inputm_hotkey, "alt-shift") ? "Alt + Shift" : "Win + Space",
                        active_drop == DROP_INPUTM_HOTKEY,
                        current_user.username[0] ? 0 : RELIEFOS_UI_BUTTON_DISABLED);
    reliefos_ui_text(ui, 372, 266, T("Candidates"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 470, 266, T("System overlay"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    if (inputm_option_count) {
        for (uint32_t i = 0; i < inputm_option_count; ++i) {
            char label[SETTINGS_INPUTM_OPTION_LABEL_LEN + 12U];
            uint32_t pos = 0;
            const char *base = T(inputm_options[i].label);
            label[0] = 0;
            append_text(label, &pos, sizeof(label), base);
            append_text(label, &pos, sizeof(label), inputm_options[i].value ? ": On" : ": Off");
            reliefos_ui_button(ui, 44U + i * 150U, 304, 140, RELIEFOS_UI_BUTTON_H,
                             label, current_user.username[0] ?
                                        (inputm_options[i].value ? RELIEFOS_UI_BUTTON_PRESSED : 0) :
                                        RELIEFOS_UI_BUTTON_DISABLED);
        }
    } else {
        reliefos_ui_text(ui, 44, 314, T("This input method has no configurable options."),
                       RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    }
    reliefos_ui_button(ui, 510, 344, 156, RELIEFOS_UI_BUTTON_H,
                     selected && selected->settings_app[0] ?
                         T("Open provider settings") :
                         T("Update dictionary"),
                     !selected ||
                         (!(selected->settings_app[0]) &&
                          (!text_eq(selected->id, "oschinpt") || !selected->path[0])) ?
                         RELIEFOS_UI_BUTTON_DISABLED : 0);
    if (active_drop == DROP_INPUTM_STARTUP && selected) {
        reliefos_ui_dropdown(ui, 510, 184, 156, startup_items, 3,
                           selected->startup_mode, SETTINGS_DROPDOWN_ROW_H, 1000);
    } else if (active_drop == DROP_INPUTM_HOTKEY) {
        reliefos_ui_dropdown(ui, 170, 284, 180, hotkey_items, 2,
                           text_eq(inputm_hotkey, "alt-shift") ? 1U : 0U,
                           SETTINGS_DROPDOWN_ROW_H, 1000);
    }
}

static void draw_users_page(struct reliefos_ui_surface *ui)
{
    const struct reliefos_ui_list_column cols[] = {
        {T("User"), 180},
        {T("Role"), 130},
        {T("State"), 120},
    };
    char state[32];
    reliefos_ui_text(ui, 34, 64,
                   current_user.role == RELIEFOS_AUTH_ROLE_ADMIN
                       ? T("Administrators can create and manage local accounts.")
                       : T("You can change your password."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_listview_header(ui, 34, 98, 430, cols, 3);
    for (uint32_t row = 0; user_scroll + row < user_count && row < SETTINGS_USER_ROWS; ++row) {
        uint32_t i = user_scroll + row;
        const char *cells[3];
        copy_text(state, sizeof(state),
                  (users[i].flags & RELIEFOS_AUTH_USER_DISABLED)
                      ? T("Disabled")
                      : T("Enabled"));
        cells[0] = users[i].username;
        cells[1] = role_label(users[i].role);
        cells[2] = state;
        reliefos_ui_listview_row(ui, 34, 126 + row * 28, 430, cols, cells, 3,
                               i == selected_user ? RELIEFOS_UI_MENU_SELECTED : 0);
    }
    if (current_user.role == RELIEFOS_AUTH_ROLE_ADMIN) {
        reliefos_ui_button(ui, 484, 98, 104, RELIEFOS_UI_BUTTON_H, T("New User"), 0);
        reliefos_ui_button(ui, 484, 178, 104, RELIEFOS_UI_BUTTON_H,
                         users[selected_user].flags & RELIEFOS_AUTH_USER_DISABLED
                             ? T("Enable")
                             : T("Disable"),
                         user_count && users[selected_user].uid ? 0 : RELIEFOS_UI_BUTTON_DISABLED);
        reliefos_ui_button(ui, 484, 258, 104, RELIEFOS_UI_BUTTON_H, T("Reset Pass"),
                         user_count ? 0 : RELIEFOS_UI_BUTTON_DISABLED);
    }
    reliefos_ui_button(ui, 34, 324, 150, RELIEFOS_UI_BUTTON_H, T("Change Password"),
                     current_user.username[0] ? 0 : RELIEFOS_UI_BUTTON_DISABLED);
}

static void draw_assoc_page(struct reliefos_ui_surface *ui)
{
    const struct reliefos_ui_list_column cols[] = {
        {T("Extension"), 82},
        {T("Type"), 170},
        {T("Default app"), 160},
    };
    uint32_t candidate_indices[SETTINGS_ASSOC_CANDIDATE_MAX];
    settings_ensure_assoc_apps();
    reliefos_ui_text(ui, 34, 64,
                   T("Choose the default app used by File Manager and desktop shortcuts."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_listview_header(ui, 34, 96, 412, cols, 3);
    for (uint32_t i = 0; i < SETTINGS_ASSOC_ROWS; ++i) {
        char fake[RELIEFOS_FS_PATH_LEN];
        const char *program;
        const char *cells[3];
        uint32_t y = 124 + i * 40;
        fake_path_for_extension(fake, sizeof(fake), assoc_rows[i].extension);
        program = reliefos_launch_resolve_default_app_for_path(fake);
        cells[0] = assoc_rows[i].extension;
        cells[1] = T(assoc_rows[i].description);
        cells[2] = program_label(program);
        reliefos_ui_listview_row(ui, 34, y, 412, cols, cells, 3, 0);
        uint32_t candidate_count = settings_assoc_row_candidates(
            i, candidate_indices, SETTINGS_ASSOC_CANDIDATE_MAX);
        for (uint32_t slot = 0; slot < SETTINGS_ASSOC_CANDIDATE_MAX; ++slot) {
            const char *label = "";
            uint32_t flags = RELIEFOS_UI_BUTTON_DISABLED;
            if (slot < candidate_count) {
                const struct reliefos_app_info *candidate =
                    &settings_assoc_apps[candidate_indices[slot]];
                label = candidate->name;
                flags = text_eq(program, candidate->exec)
                            ? RELIEFOS_UI_BUTTON_PRESSED : 0;
            }
            reliefos_ui_button(ui, settings_assoc_button_x[slot], y + 2,
                             settings_assoc_button_w[slot], RELIEFOS_UI_BUTTON_H,
                             label, flags);
        }
    }
}

static void draw_services_page(struct reliefos_ui_surface *ui)
{
    reliefos_ui_text(ui, 34, 64,
                   T("Startup policy is saved here; Service Manager shows runtime state."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    for (uint32_t i = 0; i < SETTINGS_SERVICE_ROWS; ++i) {
        uint32_t y = 98 + i * 48;
        uint32_t flags = (service_rows[i].locked ||
                          current_user.role != RELIEFOS_AUTH_ROLE_ADMIN)
                             ? RELIEFOS_UI_BUTTON_DISABLED
                             : 0;
        reliefos_ui_panel(ui, 34, y, SETTINGS_W - 68, 40, RELIEFOS_UI_WHITE);
        reliefos_ui_checkbox(ui, 44, y + 9,
                           T(service_rows[i].name),
                           service_rows[i].enabled, flags);
        reliefos_ui_text_clipped(ui, 274, y + 11, SETTINGS_W - 318,
                               i == 4U ? ntp_runtime_detail :
                                         T(service_rows[i].detail),
                               service_rows[i].locked ? RELIEFOS_UI_DARK : RELIEFOS_UI_BLACK,
                               RELIEFOS_UI_WHITE);
    }
    reliefos_ui_text(ui, 34, 346, T("NTP runtime state:"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_text_clipped(ui, 174, 346, SETTINGS_W - 208, ntp_runtime_state,
                           text_eq(ntp_runtime_state, "failed") ? 0x00b03030U :
                           text_eq(ntp_runtime_state, "running") ? 0x00108040U :
                           RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);
    reliefos_ui_button(ui, 34, SETTINGS_H - 66, 108, RELIEFOS_UI_BUTTON_H,
                     T("Save"),
                     current_user.role == RELIEFOS_AUTH_ROLE_ADMIN
                         ? 0
                         : RELIEFOS_UI_BUTTON_DISABLED);
}

static void draw_activation_page(struct reliefos_ui_surface *ui)
{
    struct reliefos_license_info info;
    const char *status;
    uint32_t required;
    uint32_t ok;
    required = (uint32_t)reliefos_license_required();
    if (!required) {
        info = (struct reliefos_license_info){0};
        copy_text(info.detail, sizeof(info.detail), T("License validation is disabled for this build."));
        status = T("Not required");
        ok = 1;
    } else if (reliefos_license_status(&info) < 0) {
        info = (struct reliefos_license_info){0};
        info.status = RELIEFOS_LICENSE_STATUS_INVALID;
        copy_text(info.detail, sizeof(info.detail), "license status unavailable");
        status = license_status_label(info.status);
        ok = 0;
    } else {
        status = license_status_label(info.status);
        ok = info.status == RELIEFOS_LICENSE_STATUS_OK;
    }
    reliefos_ui_text(ui, 34, 64, T("Activation"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_panel(ui, 34, 94, SETTINGS_W - 68, 52,
                    ok ? RELIEFOS_UI_WHITE : RELIEFOS_UI_LIGHT);
    reliefos_ui_text(ui, 50, 110, T("Computer"),
                   RELIEFOS_UI_BLACK, ok ? RELIEFOS_UI_WHITE : RELIEFOS_UI_LIGHT);
    reliefos_ui_text_clipped(ui, 176, 110, SETTINGS_W - 226,
                           status,
                           ok ? RELIEFOS_UI_BLACK : RELIEFOS_UI_DARK,
                           ok ? RELIEFOS_UI_WHITE : RELIEFOS_UI_LIGHT);
    draw_field(ui, 170, T("Activation mode"),
               !required ? T("Not required") :
               ok ? license_mode_label(info.mode) : "-");
    draw_field(ui, 202, T("Machine ID"), info.install_id);
    draw_field(ui, 234, T("Email hash"), info.email_hash);
    draw_field(ui, 266, T("Detail"), info.detail);
    draw_field(ui, 298, T("License file"), RELIEFOS_PATH_LICENSE);
}

static void draw_settings(struct reliefos_ui_surface *ui)
{
    struct reliefos_ui_tab_item tabs[] = {
        {T("Display"), PAGE_DISPLAY, 0},
        {T("Personalize"), PAGE_PERSONALIZATION, 0},
        {T("Users"), PAGE_USERS, 0},
        {T("File Types"), PAGE_ASSOC, 0},
        {T("Services"), PAGE_SERVICES, 0},
        {T("Activation"), PAGE_ACTIVATION, 0},
        {T("Input Method"), PAGE_INPUT_METHODS, 0},
        {T("Sound"), PAGE_SOUND, 0},
    };
    reliefos_ui_rect(ui, 0, 0, SETTINGS_W, SETTINGS_H, RELIEFOS_UI_GRAY);
    settings_tabs.selected_id = active_page;
    reliefos_ui_tab_control(ui, 18, SETTINGS_TAB_Y, SETTINGS_W - 36, tabs,
                          SETTINGS_TAB_COUNT, &settings_tabs);
    reliefos_ui_tab_body(ui, 18, SETTINGS_BODY_Y, SETTINGS_W - 36, SETTINGS_H - 84);
    if (active_page == PAGE_DISPLAY) {
        draw_display_page(ui);
    } else if (active_page == PAGE_PERSONALIZATION) {
        draw_personalization_page(ui);
    } else if (active_page == PAGE_USERS) {
        draw_users_page(ui);
    } else if (active_page == PAGE_ASSOC) {
        draw_assoc_page(ui);
    } else if (active_page == PAGE_SERVICES) {
        draw_services_page(ui);
    } else if (active_page == PAGE_INPUT_METHODS) {
        draw_input_methods_page(ui);
    } else if (active_page == PAGE_SOUND) {
        settings_sound_draw(ui, &sound_model);
    } else {
        draw_activation_page(ui);
    }
    reliefos_ui_statusbar(ui, SETTINGS_H - 28, 28, status_text);
}

static int handle_open_dropdown_hit(int32_t x, int32_t y)
{
    uint32_t id = 0;
    struct reliefos_ui_dropdown_item mode_items[SETTINGS_MODE_COUNT];
    struct reliefos_ui_dropdown_item scale_items[SETTINGS_SCALE_COUNT];
    struct reliefos_ui_dropdown_item lang_items[2];
    struct reliefos_ui_dropdown_item theme_items[2];
    struct reliefos_ui_dropdown_item metro_items[RELIEFOS_UI_COLOR_SCHEME_COUNT];
    struct reliefos_ui_dropdown_item win95_items[RELIEFOS_UI_COLOR_SCHEME_COUNT];
    struct reliefos_ui_dropdown_item wallpaper_items[RELIEFOS_WALLPAPER_MODE_COUNT];
    for (uint32_t i = 0; i < SETTINGS_MODE_COUNT; ++i) {
        mode_items[i].label = mode_labels[i];
        mode_items[i].id = i;
        mode_items[i].flags = mode_supported(i, display_state.scale_index) ? 0 : RELIEFOS_UI_MENU_DISABLED;
    }
    for (uint32_t i = 0; i < SETTINGS_SCALE_COUNT; ++i) {
        scale_items[i].label = scale_labels[i];
        scale_items[i].id = i;
        scale_items[i].flags = mode_supported(display_state.mode_index, i) ? 0 : RELIEFOS_UI_MENU_DISABLED;
    }
    lang_items[0] = (struct reliefos_ui_dropdown_item){language_options[0].name, 0, 0};
    lang_items[1] = (struct reliefos_ui_dropdown_item){language_options[1].name, 1, 0};
    theme_items[0] = (struct reliefos_ui_dropdown_item){"Metro", RELIEFOS_UI_THEME_METRO, 0};
    theme_items[1] = (struct reliefos_ui_dropdown_item){"Win95", RELIEFOS_UI_THEME_WIN95, 0};
    fill_theme_color_items(metro_items);
    fill_theme_color_items(win95_items);
    fill_wallpaper_mode_items(wallpaper_items);
    if (active_drop == DROP_RESOLUTION &&
        reliefos_ui_dropdown_hit(x, y, 160, 122, 190, mode_items, SETTINGS_MODE_COUNT,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (id < SETTINGS_MODE_COUNT && mode_supported(id, display_state.scale_index)) {
            request_display(RELIEFOS_DISPLAY_REQUEST_APPLY, id, display_state.scale_index);
            copy_text(status_text, sizeof(status_text), T("Resolution changed"));
        }
        return 1;
    }
    if (active_drop == DROP_SCALE &&
        reliefos_ui_dropdown_hit(x, y, 160, 162, 190, scale_items, SETTINGS_SCALE_COUNT,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (id < SETTINGS_SCALE_COUNT && mode_supported(display_state.mode_index, id)) {
            request_display(RELIEFOS_DISPLAY_REQUEST_APPLY, display_state.mode_index, id);
            copy_text(status_text, sizeof(status_text), T("Scale changed"));
        }
        return 1;
    }
    if (active_drop == DROP_LANGUAGE &&
        reliefos_ui_dropdown_hit(x, y, 160, 202, 190, lang_items, 2,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (id < 2) {
            if (reliefos_environment_set(RELIEFOS_ENV_SCOPE_USER, "LANG",
                                       language_options[id].locale) == 0) {
                selected_language = id;
                copy_text(status_text, sizeof(status_text), T("Reopen applications to apply the language"));
            } else {
                copy_text(status_text, sizeof(status_text), T("Could not save language settings"));
            }
        }
        return 1;
    }
    if (active_drop == DROP_THEME &&
        reliefos_ui_dropdown_hit(x, y, 160, 122, 190, theme_items, 2,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (current_user.username[0] &&
            (id == RELIEFOS_UI_THEME_METRO || id == RELIEFOS_UI_THEME_WIN95)) {
            appearance_state.theme = id;
            request_appearance_change(T("Theme style changed"));
        }
        return 1;
    }
    if (active_drop == DROP_METRO_COLOR &&
        reliefos_ui_dropdown_hit(x, y, 160, 162, 190, metro_items,
                               RELIEFOS_UI_COLOR_SCHEME_COUNT,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (current_user.username[0] && id < RELIEFOS_UI_COLOR_SCHEME_COUNT) {
            appearance_state.metro_color_scheme = id;
            request_appearance_change(T("Metro color changed"));
        }
        return 1;
    }
    if (active_drop == DROP_WIN95_COLOR &&
        reliefos_ui_dropdown_hit(x, y, 160, 202, 190, win95_items,
                               RELIEFOS_UI_COLOR_SCHEME_COUNT,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (current_user.username[0] && id < RELIEFOS_UI_COLOR_SCHEME_COUNT) {
            appearance_state.win95_color_scheme = id;
            request_appearance_change(T("Win95 color changed"));
        }
        return 1;
    }
    if (active_drop == DROP_WALLPAPER_MODE &&
        reliefos_ui_dropdown_hit(x, y, 160, 282, 190, wallpaper_items,
                               RELIEFOS_WALLPAPER_MODE_COUNT,
                               SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
        active_drop = DROP_NONE;
        if (current_user.username[0] && id < RELIEFOS_WALLPAPER_MODE_COUNT) {
            appearance_state.wallpaper_mode = id;
            request_appearance_change(T("Wallpaper mode changed"));
        }
        return 1;
    }
    return 0;
}

static void create_user_dialog(uint32_t role)
{
    char name[RELIEFOS_AUTH_USERNAME_LEN] = "";
    char pass[RELIEFOS_AUTH_PASSWORD_LEN] = "";
    struct reliefos_user_info user;
    if (reliefos_ui_show_input_dialog(T("Create user"), T("Username"),
                                    name, sizeof(name)) <= 0) {
        return;
    }
    if (reliefos_ui_show_password_dialog(T("Create user"), T("Password"),
                                       pass, sizeof(pass)) <= 0) {
        explicit_bzero(pass, sizeof(pass));
        return;
    }
    if (reliefos_auth_create_user(name, pass, role, &user) == 0) {
        copy_text(status_text, sizeof(status_text), T("User created"));
        refresh_users();
    } else {
        copy_text(status_text, sizeof(status_text), T("Could not create user"));
    }
    explicit_bzero(pass, sizeof(pass));
}

static void reset_password_dialog(uint32_t uid)
{
    struct passwd *account = getpwuid(uid);
    if (!account) return;
    char *args[] = {"/usr/lib/reliefos/apps/terminal/terminal.elf", "-e",
                     "/usr/bin/passwd", account->pw_name, NULL};
    if (reliefos_spawn_argv(args[0], args) < 0)
        copy_text(status_text, sizeof(status_text), T("Could not start passwd"));
}

static void change_my_password(void)
{
    reset_password_dialog(getuid());
}

static void handle_users_click(int32_t x, int32_t y)
{
    for (uint32_t row = 0; user_scroll + row < user_count && row < SETTINGS_USER_ROWS; ++row) {
        if (hit_rect_i(x, y, 34, 126 + (int32_t)row * 28, 430, 28)) {
            selected_user = user_scroll + row;
            return;
        }
    }
    if (current_user.role == RELIEFOS_AUTH_ROLE_ADMIN) {
        if (hit_rect_i(x, y, 484, 98, 104, RELIEFOS_UI_BUTTON_H)) {
            create_user_dialog(RELIEFOS_AUTH_ROLE_USER);
        } else if (user_count && users[selected_user].uid && hit_rect_i(x, y, 484, 178, 104, RELIEFOS_UI_BUTTON_H)) {
            uint32_t flags = users[selected_user].flags ^ RELIEFOS_AUTH_USER_DISABLED;
            if (reliefos_auth_update_user(users[selected_user].uid, RELIEFOS_AUTH_UPDATE_FLAGS,
                                        users[selected_user].role, flags) == 0) {
                copy_text(status_text, sizeof(status_text), T("User state updated"));
            } else {
                copy_text(status_text, sizeof(status_text), T("User state change denied"));
            }
            refresh_users();
        } else if (user_count && hit_rect_i(x, y, 484, 258, 104, RELIEFOS_UI_BUTTON_H)) {
            reset_password_dialog(users[selected_user].uid);
        }
    }
    if (hit_rect_i(x, y, 34, 324, 150, RELIEFOS_UI_BUTTON_H)) {
        change_my_password();
    }
}

static void handle_display_click(int32_t x, int32_t y)
{
    if (active_drop && handle_open_dropdown_hit(x, y)) {
        return;
    }
    active_drop = DROP_NONE;
    if (hit_rect_i(x, y, 160, 98, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_RESOLUTION;
        return;
    }
    if (hit_rect_i(x, y, 160, 138, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_SCALE;
        return;
    }
    if (hit_rect_i(x, y, 370, 138, 150, RELIEFOS_UI_BUTTON_H)) {
        uint32_t next = display_state.scale_index;
        if (reliefos_ui_slider_handle_mouse(&next,
                                          SETTINGS_SCALE_COUNT > 1 ? SETTINGS_SCALE_COUNT - 1 : 1,
                                          370, 138, 150, RELIEFOS_UI_BUTTON_H,
                                          x, y) &&
            next < SETTINGS_SCALE_COUNT &&
            mode_supported(display_state.mode_index, next)) {
            request_display(RELIEFOS_DISPLAY_REQUEST_APPLY,
                            display_state.mode_index, next);
            copy_text(status_text, sizeof(status_text), T("Scale changed"));
        }
        return;
    }
    if (hit_rect_i(x, y, 160, 178, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_LANGUAGE;
        return;
    }
    if (display_state.pending_confirm && hit_rect_i(x, y, 54, 306, 82, RELIEFOS_UI_BUTTON_H)) {
        request_display(RELIEFOS_DISPLAY_REQUEST_KEEP, display_state.mode_index, display_state.scale_index);
        copy_text(status_text, sizeof(status_text), T("Display settings saved"));
        return;
    }
    if (display_state.pending_confirm && hit_rect_i(x, y, 146, 306, 82, RELIEFOS_UI_BUTTON_H)) {
        request_display(RELIEFOS_DISPLAY_REQUEST_REVERT, display_state.mode_index, display_state.scale_index);
        copy_text(status_text, sizeof(status_text), T("Display settings reverted"));
    }
}

static void choose_wallpaper_dialog(void)
{
    char path[RELIEFOS_FS_PATH_LEN];
    copy_text(path, sizeof(path),
              appearance_state.wallpaper_path[0]
                  ? appearance_state.wallpaper_path
                  : SETTINGS_DEFAULT_WALLPAPER_PATH);
    if (reliefos_ui_show_open_dialog(T("Choose wallpaper"),
                                   path, sizeof(path),
                                   T("Bitmap (*.bmp)"),
                                   ".bmp") <= 0) {
        return;
    }
    if (!validate_wallpaper_bmp(path)) {
        copy_text(status_text, sizeof(status_text),
                  T("Wallpaper BMP must be uncompressed 24/32-bit and no larger than 1280 x 720."));
        return;
    }
    copy_text(appearance_state.wallpaper_path,
              sizeof(appearance_state.wallpaper_path), path);
    request_appearance_change(T("Wallpaper changed"));
}

static void handle_personalization_click(int32_t x, int32_t y)
{
    if (active_drop && handle_open_dropdown_hit(x, y)) {
        return;
    }
    active_drop = DROP_NONE;
    if (!current_user.username[0]) {
        copy_text(status_text, sizeof(status_text),
                  T("Sign in to change personalization."));
        return;
    }
    if (hit_rect_i(x, y, 160, 98, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_THEME;
        return;
    }
    if (hit_rect_i(x, y, 160, 138, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_METRO_COLOR;
        return;
    }
    if (hit_rect_i(x, y, 160, 178, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_WIN95_COLOR;
        return;
    }
    if (hit_rect_i(x, y, 486, 218, 82, RELIEFOS_UI_BUTTON_H)) {
        choose_wallpaper_dialog();
        return;
    }
    if (hit_rect_i(x, y, 576, 218, 82, RELIEFOS_UI_BUTTON_H)) {
        copy_text(appearance_state.wallpaper_path,
                  sizeof(appearance_state.wallpaper_path),
                  SETTINGS_DEFAULT_WALLPAPER_PATH);
        request_appearance_change(T("Default wallpaper restored"));
        return;
    }
    if (hit_rect_i(x, y, 160, 258, 190, RELIEFOS_FONT_H + 8)) {
        active_drop = DROP_WALLPAPER_MODE;
        return;
    }
}

static void set_assoc_for_row(uint32_t row, const char *program)
{
    int ret;
    if (row >= SETTINGS_ASSOC_ROWS) {
        return;
    }
    ret = reliefos_launch_set_extension_association(assoc_rows[row].extension,
                                                  program);
    if (ret == 0) {
        copy_text(status_text, sizeof(status_text),
                  T("File association saved"));
    } else {
        copy_text(status_text, sizeof(status_text),
                  T("Could not save file association"));
    }
}

static void handle_assoc_click(int32_t x, int32_t y)
{
    uint32_t candidate_indices[SETTINGS_ASSOC_CANDIDATE_MAX];
    settings_ensure_assoc_apps();
    for (uint32_t i = 0; i < SETTINGS_ASSOC_ROWS; ++i) {
        int32_t row_y = 124 + (int32_t)i * 40;
        uint32_t candidate_count = settings_assoc_row_candidates(
            i, candidate_indices, SETTINGS_ASSOC_CANDIDATE_MAX);
        for (uint32_t slot = 0; slot < candidate_count; ++slot) {
            if (hit_rect_i(x, y, (int32_t)settings_assoc_button_x[slot],
                           row_y + 2, settings_assoc_button_w[slot],
                           RELIEFOS_UI_BUTTON_H)) {
                set_assoc_for_row(i, settings_assoc_apps[candidate_indices[slot]].exec);
                return;
            }
        }
    }
}

static void handle_services_click(int32_t x, int32_t y)
{
    for (uint32_t i = 0; i < SETTINGS_SERVICE_ROWS; ++i) {
        int32_t row_y = 98 + (int32_t)i * 48;
        if (current_user.role == RELIEFOS_AUTH_ROLE_ADMIN &&
            !service_rows[i].locked &&
            hit_rect_i(x, y, 44, row_y + 4, 220, 32)) {
            service_rows[i].enabled = service_rows[i].enabled ? 0 : 1;
            copy_text(status_text, sizeof(status_text),
                      T("Service setting changed"));
            return;
        }
    }
    if (hit_rect_i(x, y, 34, SETTINGS_H - 66, 108, RELIEFOS_UI_BUTTON_H)) {
        save_services_config();
    }
}

static void inputm_set_status(int ok, const char *success, const char *failure)
{
    copy_text(status_text, sizeof(status_text), ok ? success : failure);
}

static void inputm_request_login_start(const struct settings_inputm_entry *entry)
{
    struct reliefos_startup_command command = {0};
    uint32_t request_id = 0;
    if (!entry || !entry->path[0] || text_len(entry->path) >= sizeof(command.path)) {
        return;
    }
    copy_text(command.path, sizeof(command.path), entry->path);
    if (reliefos_startup_request(&command, &request_id) == 0) {
        copy_text(status_text, sizeof(status_text),
                  T("Login startup approval requested"));
    }
}

static void handle_input_methods_click(int32_t x, int32_t y)
{
    struct settings_inputm_entry *entry =
        inputm_selected < inputm_entry_count ? &inputm_entries[inputm_selected] : 0;
    if (active_drop == DROP_INPUTM_STARTUP && entry) {
        struct reliefos_ui_dropdown_item items[3] = {
            {T("Manual"), TEXT_INPUT_START_MANUAL, 0},
            {T("At sign-in"), TEXT_INPUT_START_LOGIN, 0},
            {T("On demand"), TEXT_INPUT_START_ON_DEMAND, 0},
        };
        uint32_t id = 0;
        if (reliefos_ui_dropdown_hit(x, y, 510, 184, 156, items, 3,
                                   SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
            char key[48];
            char value[4];
            active_drop = DROP_NONE;
            if (id <= TEXT_INPUT_START_ON_DEMAND) {
                entry->startup_mode = id;
                inputm_provider_key(key, sizeof(key), entry->config_index, "startup");
                value[0] = (char)('0' + id);
                value[1] = 0;
                inputm_set_status(inputm_append_config(key, value),
                                  T("Startup behavior saved"),
                                  T("Could not save input method"));
                if (id == TEXT_INPUT_START_LOGIN) {
                    inputm_request_login_start(entry);
                }
            }
            return;
        }
    }
    if (active_drop == DROP_INPUTM_HOTKEY) {
        struct reliefos_ui_dropdown_item items[2] = {
            {"Win + Space", 0, 0},
            {"Alt + Shift", 1, 0},
        };
        uint32_t id = 0;
        if (reliefos_ui_dropdown_hit(x, y, 170, 284, 180, items, 2,
                                   SETTINGS_DROPDOWN_ROW_H, 1000, &id)) {
            active_drop = DROP_NONE;
            copy_text(inputm_hotkey, sizeof(inputm_hotkey),
                      id == 1U ? "alt-shift" : "win-space");
            inputm_set_status(inputm_append_config("inputm_hotkey", inputm_hotkey),
                              T("Input shortcut changed"),
                              T("Could not save input method"));
            return;
        }
    }
    active_drop = DROP_NONE;
    if (!current_user.username[0]) {
        copy_text(status_text, sizeof(status_text),
                  T("Sign in to change input methods"));
        return;
    }
    if (y >= 118 && y < 118 + (int32_t)(inputm_entry_count * 27U) &&
        x >= 34 && x < 494) {
        inputm_selected = (uint32_t)(y - 118) / 27U;
        inputm_reload_extension_options();
        return;
    }
    entry = inputm_selected < inputm_entry_count ? &inputm_entries[inputm_selected] : 0;
    if (!entry) {
        return;
    }
    if (hit_rect_i(x, y, 510, 92, 156, RELIEFOS_UI_BUTTON_H) && inputm_selected != 0) {
        char key[48];
        entry->enabled = entry->enabled ? 0 : 1;
        inputm_provider_key(key, sizeof(key), entry->config_index, "enabled");
        inputm_set_status(inputm_append_config(key, entry->enabled ? "1" : "0"),
                          entry->enabled ? T("Input method enabled") :
                                           T("Input method disabled"),
                          T("Could not save input method"));
        if (!entry->enabled && text_eq(inputm_default, entry->id)) {
            copy_text(inputm_default, sizeof(inputm_default), "en");
            (void)inputm_append_config("default", "en");
        }
        if (!entry->enabled) {
            (void)text_input_set_active(current_user.uid, "en");
        }
        return;
    }
    if (hit_rect_i(x, y, 510, 126, 156, RELIEFOS_UI_BUTTON_H) && entry->enabled) {
        copy_text(inputm_default, sizeof(inputm_default), entry->id);
        inputm_set_status(inputm_append_config("default", entry->id),
                          T("Default input method saved"),
                          T("Could not save input method"));
        return;
    }
    if (hit_rect_i(x, y, 510, 160, 156, RELIEFOS_UI_BUTTON_H) && inputm_selected != 0) {
        active_drop = DROP_INPUTM_STARTUP;
        return;
    }
    if (hit_rect_i(x, y, 510, 194, 74, RELIEFOS_UI_BUTTON_H) && inputm_selected > 1U) {
        struct settings_inputm_entry *previous = &inputm_entries[inputm_selected - 1U];
        char key[48];
        char value[12];
        uint32_t pos = 0;
        uint32_t order = entry->order;
        entry->order = previous->order;
        previous->order = order;
        inputm_provider_key(key, sizeof(key), entry->config_index, "order");
        value[0] = 0;
        append_dec(value, &pos, sizeof(value), entry->order);
        if (!inputm_append_config(key, value)) {
            copy_text(status_text, sizeof(status_text),
                      T("Could not save input method order"));
            return;
        }
        inputm_provider_key(key, sizeof(key), previous->config_index, "order");
        pos = 0;
        value[0] = 0;
        append_dec(value, &pos, sizeof(value), previous->order);
        if (!inputm_append_config(key, value)) {
            copy_text(status_text, sizeof(status_text),
                      T("Could not save input method order"));
            return;
        }
        inputm_sort_entries();
        --inputm_selected;
        inputm_reload_extension_options();
        copy_text(status_text, sizeof(status_text), T("Input method moved"));
        return;
    }
    if (hit_rect_i(x, y, 592, 194, 74, RELIEFOS_UI_BUTTON_H) && inputm_selected != 0 &&
        inputm_selected + 1U < inputm_entry_count) {
        struct settings_inputm_entry *next = &inputm_entries[inputm_selected + 1U];
        char key[48];
        char value[12];
        uint32_t pos = 0;
        uint32_t order = entry->order;
        entry->order = next->order;
        next->order = order;
        inputm_provider_key(key, sizeof(key), entry->config_index, "order");
        value[0] = 0;
        append_dec(value, &pos, sizeof(value), entry->order);
        if (!inputm_append_config(key, value)) {
            copy_text(status_text, sizeof(status_text),
                      T("Could not save input method order"));
            return;
        }
        inputm_provider_key(key, sizeof(key), next->config_index, "order");
        pos = 0;
        value[0] = 0;
        append_dec(value, &pos, sizeof(value), next->order);
        if (!inputm_append_config(key, value)) {
            copy_text(status_text, sizeof(status_text),
                      T("Could not save input method order"));
            return;
        }
        inputm_sort_entries();
        ++inputm_selected;
        inputm_reload_extension_options();
        copy_text(status_text, sizeof(status_text), T("Input method moved"));
        return;
    }
    if (hit_rect_i(x, y, 170, 260, 180, RELIEFOS_UI_BUTTON_H)) {
        active_drop = DROP_INPUTM_HOTKEY;
        return;
    }
    for (uint32_t i = 0; i < inputm_option_count; ++i) {
        if (hit_rect_i(x, y, 44 + (int32_t)i * 150, 304, 140, RELIEFOS_UI_BUTTON_H)) {
            inputm_options[i].value = inputm_options[i].value ? 0 : 1;
            inputm_set_status(inputm_append_config(inputm_options[i].key,
                                                   inputm_options[i].value ? "1" : "0"),
                              T("Input method setting changed"),
                              T("Could not save input method"));
            return;
        }
    }
    if (hit_rect_i(x, y, 510, 344, 156, RELIEFOS_UI_BUTTON_H) && entry->settings_app[0]) {
        char *argv[2];
        argv[0] = entry->settings_app;
        argv[1] = 0;
        inputm_set_status(reliefos_spawn_argv(entry->settings_app, argv) > 0,
                          T("Provider settings started"),
                          T("Could not start provider settings"));
        return;
    }
    if (hit_rect_i(x, y, 510, 344, 156, RELIEFOS_UI_BUTTON_H) &&
        text_eq(entry->id, "oschinpt") && entry->path[0]) {
        char *argv[3];
        argv[0] = entry->path;
        argv[1] = "--update";
        argv[2] = 0;
        inputm_set_status(reliefos_spawn_argv(entry->path, argv) > 0,
                          T("Dictionary update started"),
                          T("Could not start dictionary update"));
    }
}

static void handle_click(int32_t x, int32_t y)
{
    struct reliefos_ui_tab_item tabs[] = {
        {T("Display"), PAGE_DISPLAY, 0},
        {T("Personalize"), PAGE_PERSONALIZATION, 0},
        {T("Users"), PAGE_USERS, 0},
        {T("File Types"), PAGE_ASSOC, 0},
        {T("Services"), PAGE_SERVICES, 0},
        {T("Activation"), PAGE_ACTIVATION, 0},
        {T("Input Method"), PAGE_INPUT_METHODS, 0},
        {T("Sound"), PAGE_SOUND, 0},
    };
    if (reliefos_ui_tab_control_handle_mouse(&settings_tabs, x, y, 18,
                                           SETTINGS_TAB_Y, SETTINGS_W - 36,
                                           tabs, SETTINGS_TAB_COUNT)) {
        active_page = (uint8_t)settings_tabs.selected_id;
        active_drop = DROP_NONE;
        return;
    }
    if (active_page == PAGE_DISPLAY) {
        handle_display_click(x, y);
    } else if (active_page == PAGE_PERSONALIZATION) {
        handle_personalization_click(x, y);
    } else if (active_page == PAGE_USERS) {
        handle_users_click(x, y);
    } else if (active_page == PAGE_ASSOC) {
        handle_assoc_click(x, y);
    } else if (active_page == PAGE_SERVICES) {
        handle_services_click(x, y);
    } else if (active_page == PAGE_INPUT_METHODS) {
        handle_input_methods_click(x, y);
    } else if (active_page == PAGE_SOUND) {
        settings_sound_click(x, y, &sound_model);
    }
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    selected_language = language_selection();
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    unsigned long last_refresh = 0;
    puts("[settings.elf] settings starting");
    reliefos_ui_tab_state_init(&settings_tabs, PAGE_DISPLAY);
    window_id = reliefos_gui_create_app_window_ex(T("Settings"),
                                                T("System settings"),
                                                SETTINGS_W, SETTINGS_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[settings.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, SETTINGS_W, SETTINGS_H, SETTINGS_W);
    refresh_display_state();
    refresh_appearance_state();
    refresh_users();
    inputm_load_settings();
    (void)settings_sound_init();
    settings_sound_poll(&sound_model);
    load_services_config();
    refresh_ntp_runtime_state();
    draw_settings(&ui);
    reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
    for (;;) {
        if (poll_settings_openrc()) {
            draw_settings(&ui);
            reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
        }
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                settings_sound_shutdown();
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_THEME_CHANGED) {
                (void)reliefos_ui_theme_set_appearance((uint32_t)event.x,
                                                     (uint32_t)event.y,
                                                     (uint32_t)event.dx);
                refresh_appearance_state();
                draw_settings(&ui);
                reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 1u)) {
                handle_click(event.x, event.y);
                refresh_display_state();
                refresh_appearance_state();
                refresh_users();
                refresh_ntp_runtime_state();
                draw_settings(&ui);
                reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL && active_page == PAGE_USERS) {
                if (event.dy > 0 && user_scroll) --user_scroll;
                if (event.dy < 0 && user_scroll + SETTINGS_USER_ROWS < user_count) ++user_scroll;
                draw_settings(&ui);
                reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event.pressed &&
                event.keycode == SETTINGS_KEY_ESCAPE) {
                settings_sound_shutdown();
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_FOCUS || event.type == RELIEFOS_GUI_APP_EVENT_RESIZE) {
                refresh_display_state();
                refresh_appearance_state();
                refresh_users();
                inputm_load_settings();
                settings_sound_poll(&sound_model);
                refresh_ntp_runtime_state();
                draw_settings(&ui);
                reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
            }
        } else {
            unsigned long now = reliefos_uptime_ms();
            if (now - last_refresh >= 250) {
                refresh_display_state();
                refresh_appearance_state();
                refresh_users();
                settings_sound_poll(&sound_model);
                draw_settings(&ui);
                reliefos_gui_present_window((uint32_t)window_id, SETTINGS_W, SETTINGS_H, SETTINGS_W, pixels);
                last_refresh = now;
            }
            sleep_ms(20);
        }
    }
}
