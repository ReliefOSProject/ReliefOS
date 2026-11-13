#include <reliefos/devmgr_service.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>

#define DEVMGR_W 720
#define DEVMGR_H 420
#define DEVMGR_MAX_W RELIEFOS_GUI_MAX_WINDOW_WIDTH
#define DEVMGR_MAX_H RELIEFOS_GUI_MAX_WINDOW_HEIGHT
#define DEVMGR_ROW_H 24
#define DEVMGR_STATUS_H 28
#define DEVMGR_DETAIL_PANEL_H 96
#define DEVMGR_TOOLBAR_Y 8
#define DEVMGR_BUTTON_Y 14
#define DEVMGR_LIST_FRAME_Y 56
#define DEVMGR_LIST_HEADER_Y 58
#define DEVMGR_LIST_ROW_Y 86
#define DEVMGR_KEY_ESCAPE 1U
#define T(s) gettext(s)

static uint32_t pixels[DEVMGR_MAX_W * DEVMGR_MAX_H];
static system_device_info_t devices[SYSTEM_DEVICE_MAX];
static const char *class_text[SYSTEM_DEVICE_MAX];
static char flags_text[SYSTEM_DEVICE_MAX][48];
static uint32_t device_count;
static struct reliefos_ui_listview_state device_list;
static char status_text[128] = "Ready";
static uint32_t view_w = DEVMGR_W;
static uint32_t view_h = DEVMGR_H;

static void append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (buf && pos && *pos + 1 < cap) {
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

static void append_u64(char *buf, uint32_t *pos, uint32_t cap, uint64_t value)
{
    char tmp[24];
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

static void set_status_code(const char *prefix, int value)
{
    uint32_t pos = 0;
    status_text[0] = 0;
    append_text(status_text, &pos, sizeof(status_text), prefix);
    append_text(status_text, &pos, sizeof(status_text), " ret=");
    if (value < 0) {
        append_char(status_text, &pos, sizeof(status_text), '-');
        value = -value;
    }
    append_u64(status_text, &pos, sizeof(status_text), (uint32_t)value);
}

static const char *device_class_name(uint32_t cls)
{
    switch (cls) {
    case SYSTEM_DEVICE_CLASS_SYSTEM:
        return T("System");
    case SYSTEM_DEVICE_CLASS_INPUT:
        return T("Input");
    case SYSTEM_DEVICE_CLASS_DISPLAY:
        return T("Display");
    case SYSTEM_DEVICE_CLASS_STORAGE:
        return T("Storage");
    case SYSTEM_DEVICE_CLASS_SERIAL:
        return T("Serial");
    case SYSTEM_DEVICE_CLASS_NETWORK:
        return T("Network");
    case SYSTEM_DEVICE_CLASS_AUDIO:
        return T("Audio");
    default:
        return T("Other");
    }
}

static void format_flags(char *buf, uint32_t cap, uint32_t flags)
{
    uint32_t pos = 0;
    buf[0] = 0;
    if (flags & SYSTEM_DEVICE_FLAG_PRESENT) {
        append_text(buf, &pos, cap, T("Present"));
    }
    if (flags & SYSTEM_DEVICE_FLAG_ACTIVE) {
        if (pos) {
            append_text(buf, &pos, cap, ", ");
        }
        append_text(buf, &pos, cap, T("Active"));
    }
    if (flags & SYSTEM_DEVICE_FLAG_BOOT) {
        if (pos) {
            append_text(buf, &pos, cap, ", ");
        }
        append_text(buf, &pos, cap, T("Boot"));
    }
    if (!pos) {
        append_text(buf, &pos, cap, T("None"));
    }
}

static void refresh_devices(void)
{
    uint32_t count = SYSTEM_DEVICE_MAX;
    int ret = system_device_list(devices, SYSTEM_DEVICE_MAX, &count);
    if (ret < 0) {
        device_count = 0;
        device_list.selected = -1;
        reliefos_ui_listview_state_set_count(&device_list, 0);
        set_status_code(T("Device refresh failed"), ret);
        return;
    }
    device_count = count > SYSTEM_DEVICE_MAX ? SYSTEM_DEVICE_MAX : count;
    for (uint32_t i = 0; i < device_count; ++i) {
        class_text[i] = device_class_name(devices[i].device_class);
        format_flags(flags_text[i], sizeof(flags_text[i]), devices[i].flags);
    }
    reliefos_ui_listview_state_set_count(&device_list, device_count);
    if (device_count && device_list.selected < 0) {
        device_list.selected = 0;
    }
    if ((uint32_t)device_list.selected >= device_count) {
        device_list.selected = device_count ? (int32_t)(device_count - 1) : -1;
    }
    {
        uint32_t pos = 0;
        status_text[0] = 0;
        append_text(status_text, &pos, sizeof(status_text), T("Devices refreshed: "));
        append_u64(status_text, &pos, sizeof(status_text), device_count);
    }
}

static const system_device_info_t *selected_device(void)
{
    if (device_list.selected < 0 || (uint32_t)device_list.selected >= device_count) {
        return 0;
    }
    return &devices[device_list.selected];
}

static uint32_t details_y(void)
{
    return view_h > DEVMGR_STATUS_H + DEVMGR_DETAIL_PANEL_H + 10
               ? view_h - DEVMGR_STATUS_H - DEVMGR_DETAIL_PANEL_H - 10
               : 300;
}

static uint32_t list_frame_h(void)
{
    uint32_t bottom = details_y();
    return bottom > DEVMGR_LIST_FRAME_Y + 10 ? bottom - DEVMGR_LIST_FRAME_Y - 10 : 96;
}

static uint32_t list_scroll_h(void)
{
    uint32_t bottom = details_y();
    return bottom > DEVMGR_LIST_HEADER_Y + 12 ? bottom - DEVMGR_LIST_HEADER_Y - 12 : 80;
}

static uint32_t visible_rows(void)
{
    uint32_t bottom = details_y();
    uint32_t rows = bottom > DEVMGR_LIST_ROW_Y ? (bottom - DEVMGR_LIST_ROW_Y) / DEVMGR_ROW_H : 1;
    return rows ? rows : 1;
}

static void update_device_list_layout(void)
{
    device_list.visible_rows = visible_rows();
    reliefos_ui_listview_state_set_count(&device_list, device_count);
}

static void draw_devmgr(struct reliefos_ui_surface *ui)
{
    uint32_t panel_y = details_y();
    uint32_t frame_h = list_frame_h();
    uint32_t scroll_h = list_scroll_h();
    uint32_t list_w = view_w > 52 ? view_w - 52 : 668;
    struct reliefos_ui_list_column cols[] = {
        {T("Class"), 86},
        {T("Device"), 138},
        {T("Status"), 98},
        {T("Flags"), 128},
        {T("Details"), list_w > 86 + 138 + 98 + 128
                                  ? list_w - 86 - 138 - 98 - 128
                                  : 120},
    };
    update_device_list_layout();
    uint32_t rows = device_count > device_list.visible_rows ? device_list.visible_rows : device_count;
    const system_device_info_t *selected;
    reliefos_ui_rect(ui, 0, 0, view_w, view_h, RELIEFOS_UI_GRAY);
    reliefos_ui_toolbar(ui, 8, DEVMGR_TOOLBAR_Y, view_w > 16 ? view_w - 16 : view_w, 36);
    reliefos_ui_toolbar_button(ui, 18, DEVMGR_BUTTON_Y, 88, T("Refresh"), 0);
    reliefos_ui_text(ui, 120, DEVMGR_BUTTON_Y + 6, T("Hardware detected by the kernel"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);

    reliefos_ui_scroll_view_frame(ui, 12, DEVMGR_LIST_FRAME_Y, view_w > 24 ? view_w - 24 : view_w, frame_h);
    reliefos_ui_listview_header(ui, 14, DEVMGR_LIST_HEADER_Y, list_w, cols, 5);
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t i = device_list.scroll + row;
        const char *cells[5];
        if (i >= device_count) {
            break;
        }
        cells[0] = class_text[i];
        cells[1] = devices[i].name;
        cells[2] = devices[i].status;
        cells[3] = flags_text[i];
        cells[4] = devices[i].detail;
        reliefos_ui_listview_row(ui, 14, DEVMGR_LIST_ROW_Y + row * DEVMGR_ROW_H, list_w,
                               cols, cells, 5,
                               device_list.selected == (int32_t)i ? RELIEFOS_UI_MENU_SELECTED : 0);
    }
    reliefos_ui_vscrollbar(ui, view_w > 30 ? view_w - 30 : 690, DEVMGR_LIST_HEADER_Y, 18, scroll_h,
                         device_list.scroll,
                         device_count > device_list.visible_rows ? device_count : device_list.visible_rows,
                         device_list.visible_rows,
                         device_count <= device_list.visible_rows ? RELIEFOS_UI_SCROLLBAR_DISABLED : 0);

    reliefos_ui_panel(ui, 12, panel_y, view_w > 24 ? view_w - 24 : view_w,
                    DEVMGR_DETAIL_PANEL_H, RELIEFOS_UI_GRAY);
    selected = selected_device();
    if (selected) {
        struct reliefos_ui_property_item props[] = {
            {T("Device:"), selected->name, 0},
            {T("Status:"), selected->status, 0},
            {T("Details:"), selected->detail, 0},
        };
        reliefos_ui_property_grid(ui, 20, panel_y + 10,
                                view_w > 40 ? view_w - 40 : view_w,
                                props, sizeof(props) / sizeof(props[0]),
                                86, 22);
    } else {
        reliefos_ui_text(ui, 20, panel_y + 30, T("No device selected"),
                       RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    }
    reliefos_ui_statusbar(ui, view_h - DEVMGR_STATUS_H, DEVMGR_STATUS_H, status_text);
}

static int hit_rect_i(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("devmgr", RELIEFOS_LAYOUT_LOCALE);
    textdomain("devmgr");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    puts("[devmgr.elf] device manager starting");
    window_id = reliefos_gui_create_app_window_ex(T("Device Manager"),
                                                T("Kernel device list"),
                                                DEVMGR_W, DEVMGR_H, 0);
    if (window_id <= 0) {
        printf("[devmgr.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, view_w, view_h, DEVMGR_MAX_W);
    reliefos_ui_listview_state_init(&device_list, visible_rows(), DEVMGR_ROW_H);
    device_list.focused = 1;
    refresh_devices();
    draw_devmgr(&ui);
    reliefos_gui_present_window((uint32_t)window_id, view_w, view_h, DEVMGR_MAX_W, pixels);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        while (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 1u)) {
                if (hit_rect_i(event.x, event.y, 18, DEVMGR_BUTTON_Y, 88, RELIEFOS_UI_BUTTON_H)) {
                    refresh_devices();
                } else if (event.x >= (int32_t)(view_w > 30 ? view_w - 30 : 690) &&
                           event.y >= DEVMGR_LIST_HEADER_Y && event.y < (int32_t)(DEVMGR_LIST_HEADER_Y + list_scroll_h())) {
                    reliefos_ui_vscrollbar_handle_mouse(&device_list.scroll,
                                                      device_count > device_list.visible_rows
                                                          ? device_count
                                                          : device_list.visible_rows,
                                                      device_list.visible_rows,
                                                      view_w > 30 ? view_w - 30 : 690, DEVMGR_LIST_HEADER_Y, 18,
                                                      list_scroll_h(),
                                                      event.x, event.y);
                } else {
                    uint32_t activate = 0;
                    reliefos_ui_listview_state_handle_mouse(&device_list, event.x, event.y,
                                                          14, DEVMGR_LIST_ROW_Y,
                                                          view_w > 52 ? view_w - 52 : 668,
                                                          &activate);
                    (void)activate;
                }
                draw_devmgr(&ui);
                reliefos_gui_present_window((uint32_t)window_id, view_w, view_h, DEVMGR_MAX_W, pixels);
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL) {
                if (reliefos_ui_listview_state_handle_wheel(&device_list, event.dy)) {
                    draw_devmgr(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, view_w, view_h, DEVMGR_MAX_W, pixels);
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN) {
                uint32_t activate = 0;
                if (event.keycode == DEVMGR_KEY_ESCAPE) {
                    return 0;
                }
                if (reliefos_ui_listview_state_handle_key(&device_list, event.keycode, &activate)) {
                    draw_devmgr(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, view_w, view_h, DEVMGR_MAX_W, pixels);
                }
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE ||
                event.type == RELIEFOS_GUI_APP_EVENT_FOCUS) {
                if (event.width >= DEVMGR_W) {
                    view_w = event.width > DEVMGR_MAX_W ? DEVMGR_MAX_W : event.width;
                }
                if (event.height >= DEVMGR_H) {
                    view_h = event.height > DEVMGR_MAX_H ? DEVMGR_MAX_H : event.height;
                }
                update_device_list_layout();
                reliefos_ui_bind(&ui, pixels, view_w, view_h, DEVMGR_MAX_W);
                draw_devmgr(&ui);
                reliefos_gui_present_window((uint32_t)window_id, view_w, view_h, DEVMGR_MAX_W, pixels);
            }
        }
        sleep_ms(20);
    }
}
