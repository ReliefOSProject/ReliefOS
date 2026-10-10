#include <reliefos/devmgr_service.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>

#define DRVMGR_W 820U
#define DRVMGR_H 480U
#define DRVMGR_MAX_W RELIEFOS_GUI_MAX_WINDOW_WIDTH
#define DRVMGR_MAX_H RELIEFOS_GUI_MAX_WINDOW_HEIGHT
#define DRVMGR_ROW_H 24U
#define DRVMGR_LIST_Y 92U
#define DRVMGR_STATUS_H 28U
#define DRVMGR_KEY_ESCAPE 1U
#define T(s) gettext(s)

static uint32_t pixels[DRVMGR_MAX_W * DRVMGR_MAX_H];
static system_driver_info_t drivers[SYSTEM_DRIVER_MAX];
static struct reliefos_ui_listview_state driver_list;
static uint32_t driver_count;
static uint32_t view_w = DRVMGR_W;
static uint32_t view_h = DRVMGR_H;
static char status_text[160] = "Ready";

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t index = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[index] && index + 1U < cap) {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = 0;
}

static void append_text(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    while (src && *src && *pos + 1U < cap) {
        dst[(*pos)++] = *src++;
    }
    if (*pos < cap) {
        dst[*pos] = 0;
    }
}

static void append_u32(char *dst, uint32_t *pos, uint32_t cap, uint32_t value)
{
    char digits[16];
    uint32_t count = 0;
    if (value == 0) {
        append_text(dst, pos, cap, "0");
        return;
    }
    while (value && count < sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count) {
        char text[2] = {digits[--count], 0};
        append_text(dst, pos, cap, text);
    }
}

static int hit_rect(int32_t x, int32_t y, int32_t rx, int32_t ry,
                    int32_t width, int32_t height)
{
    return x >= rx && y >= ry && x < rx + width && y < ry + height;
}

static const char *driver_state_name(uint32_t state)
{
    switch (state) {
    case SYSTEM_DRIVER_STATE_LOADING:
        return T("Loading");
    case SYSTEM_DRIVER_STATE_LOADED:
        return T("Loaded");
    case SYSTEM_DRIVER_STATE_DISABLED:
        return T("Disabled");
    case SYSTEM_DRIVER_STATE_FAILED:
        return T("Failed");
    default:
        return T("Unloaded");
    }
}

static void set_status_code(const char *prefix, int code)
{
    uint32_t pos = 0;
    copy_text(status_text, sizeof(status_text), prefix);
    while (status_text[pos]) {
        ++pos;
    }
    append_text(status_text, &pos, sizeof(status_text), " (");
    if (code < 0) {
        append_text(status_text, &pos, sizeof(status_text), "-");
        append_u32(status_text, &pos, sizeof(status_text), (uint32_t)(-code));
    } else {
        append_u32(status_text, &pos, sizeof(status_text), (uint32_t)code);
    }
    append_text(status_text, &pos, sizeof(status_text), ")");
}

static uint32_t visible_rows(void)
{
    uint32_t bottom = view_h > DRVMGR_STATUS_H + 12U ? view_h - DRVMGR_STATUS_H - 12U : view_h;
    uint32_t rows = bottom > DRVMGR_LIST_Y ? (bottom - DRVMGR_LIST_Y) / DRVMGR_ROW_H : 1U;
    return rows ? rows : 1U;
}

static uint32_t list_height(void)
{
    uint32_t bottom = view_h > DRVMGR_STATUS_H + 12U ? view_h - DRVMGR_STATUS_H - 12U : view_h;
    return bottom > DRVMGR_LIST_Y ? bottom - DRVMGR_LIST_Y : DRVMGR_ROW_H;
}

static void refresh_drivers(void)
{
    uint32_t count = SYSTEM_DRIVER_MAX;
    int ret;
    ret = system_driver_list(drivers, SYSTEM_DRIVER_MAX, &count);
    if (ret < 0) {
        driver_count = 0;
        driver_list.selected = -1;
        reliefos_ui_listview_state_set_count(&driver_list, 0);
        set_status_code(T("Driver refresh failed"), ret);
        return;
    }
    driver_count = count > SYSTEM_DRIVER_MAX ? SYSTEM_DRIVER_MAX : count;
    reliefos_ui_listview_state_set_count(&driver_list, driver_count);
    if (driver_count && driver_list.selected < 0) {
        driver_list.selected = 0;
    }
    if ((uint32_t)driver_list.selected >= driver_count) {
        driver_list.selected = driver_count ? (int32_t)(driver_count - 1U) : -1;
    }
    copy_text(status_text, sizeof(status_text),
              T("Read-only: drivers are built into the kernel"));
}

static void draw_drvmgr(struct reliefos_ui_surface *ui)
{
    uint32_t list_w = view_w > 52U ? view_w - 52U : 668U;
    uint32_t rows = driver_count > driver_list.visible_rows ? driver_list.visible_rows : driver_count;
    struct reliefos_ui_list_column columns[] = {
        {T("File"), 132U},
        {T("Driver"), 108U},
        {T("State"), 96U},
        {T("ABI"), 54U},
        {T("Details"), list_w > 390U ? list_w - 390U : 120U},
    };
    reliefos_ui_rect(ui, 0, 0, view_w, view_h, RELIEFOS_UI_GRAY);
    reliefos_ui_toolbar(ui, 8, 8, view_w > 16U ? view_w - 16U : view_w, 70U);
    reliefos_ui_toolbar_button(ui, 18, 16, 82, T("Refresh"), 0);
    reliefos_ui_text(ui, 18, 48,
                   T("Drivers are built into the kernel image. This view is read-only."),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_GRAY);

    reliefos_ui_scroll_view_frame(ui, 12, DRVMGR_LIST_Y - 4U,
                                view_w > 24U ? view_w - 24U : view_w, list_height());
    reliefos_ui_listview_header(ui, 14, DRVMGR_LIST_Y - 2U, list_w, columns, 5U);
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t index = driver_list.scroll + row;
        const char *cells[5];
        char abi[16];
        if (index >= driver_count) {
            break;
        }
        abi[0] = 0;
        {
            uint32_t pos = 0;
            append_u32(abi, &pos, sizeof(abi), drivers[index].abi_version);
        }
        cells[0] = drivers[index].file;
        cells[1] = drivers[index].name[0] ? drivers[index].name : "-";
        cells[2] = driver_state_name(drivers[index].state);
        cells[3] = abi;
        cells[4] = drivers[index].error[0] ? drivers[index].error
                                           : (drivers[index].flags & SYSTEM_DRIVER_FLAG_DISABLED
                                                  ? T("Skipped at boot")
                                                  : T("Available"));
        reliefos_ui_listview_row(ui, 14, DRVMGR_LIST_Y + 26U + row * DRVMGR_ROW_H,
                               list_w, columns, cells, 5U,
                               driver_list.selected == (int32_t)index ? RELIEFOS_UI_MENU_SELECTED : 0);
    }
    reliefos_ui_vscrollbar(ui, view_w > 30U ? view_w - 30U : 690U, DRVMGR_LIST_Y - 2U,
                         18U, list_height() > 26U ? list_height() - 26U : 24U,
                         driver_list.scroll,
                         driver_count > driver_list.visible_rows ? driver_count : driver_list.visible_rows,
                         driver_list.visible_rows,
                         driver_count <= driver_list.visible_rows ? RELIEFOS_UI_SCROLLBAR_DISABLED : 0);
    reliefos_ui_statusbar(ui, view_h - DRVMGR_STATUS_H, DRVMGR_STATUS_H, status_text);
}

static void present(struct reliefos_ui_surface *ui, uint32_t window_id)
{
    driver_list.visible_rows = visible_rows();
    reliefos_ui_listview_state_set_count(&driver_list, driver_count);
    draw_drvmgr(ui);
    reliefos_gui_present_window(window_id, view_w, view_h, DRVMGR_MAX_W, pixels);
}

int main(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    puts("[drvmgr.elf] driver manager starting");
    window_id = reliefos_gui_create_app_window_ex(T("Driver Manager"),
                                                T("Built-in kernel drivers"),
                                                DRVMGR_W, DRVMGR_H, 0);
    if (window_id <= 0) {
        printf("[drvmgr.elf] create window failed=%d\n", window_id);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, view_w, view_h, DRVMGR_MAX_W);
    reliefos_ui_listview_state_init(&driver_list, visible_rows(), DRVMGR_ROW_H);
    driver_list.focused = 1;
    refresh_drivers();
    present(&ui, (uint32_t)window_id);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        while (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 1U)) {
                if (hit_rect(event.x, event.y, 18, 16, 82, RELIEFOS_UI_BUTTON_H)) {
                    refresh_drivers();
                } else if (event.x >= (int32_t)(view_w > 30U ? view_w - 30U : 690U) &&
                           event.y >= (int32_t)(DRVMGR_LIST_Y - 2U)) {
                    reliefos_ui_vscrollbar_handle_mouse(&driver_list.scroll,
                                                      driver_count > driver_list.visible_rows
                                                          ? driver_count : driver_list.visible_rows,
                                                      driver_list.visible_rows,
                                                      view_w > 30U ? view_w - 30U : 690U,
                                                      DRVMGR_LIST_Y - 2U, 18U,
                                                      list_height() > 26U ? list_height() - 26U : 24U,
                                                      event.x, event.y);
                } else {
                    uint32_t list_w = view_w > 52U ? view_w - 52U : 668U;
                    uint32_t activate = 0;
                    reliefos_ui_listview_state_handle_mouse(&driver_list, event.x, event.y,
                                                          14, DRVMGR_LIST_Y + 26U, list_w,
                                                          &activate);
                    (void)activate;
                }
                present(&ui, (uint32_t)window_id);
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_WHEEL) {
                if (reliefos_ui_listview_state_handle_wheel(&driver_list, event.dy)) {
                    present(&ui, (uint32_t)window_id);
                }
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN) {
                uint32_t activate = 0;
                if (event.keycode == DRVMGR_KEY_ESCAPE) {
                    return 0;
                }
                if (reliefos_ui_listview_state_handle_key(&driver_list, event.keycode, &activate)) {
                    present(&ui, (uint32_t)window_id);
                }
            } else if (event.type == RELIEFOS_GUI_APP_EVENT_RESIZE ||
                       event.type == RELIEFOS_GUI_APP_EVENT_FOCUS ||
                       event.type == RELIEFOS_GUI_APP_EVENT_THEME_CHANGED) {
                if (event.width >= DRVMGR_W) {
                    view_w = event.width > DRVMGR_MAX_W ? DRVMGR_MAX_W : event.width;
                }
                if (event.height >= DRVMGR_H) {
                    view_h = event.height > DRVMGR_MAX_H ? DRVMGR_MAX_H : event.height;
                }
                reliefos_ui_bind(&ui, pixels, view_w, view_h, DRVMGR_MAX_W);
                present(&ui, (uint32_t)window_id);
            }
        }
    }
}
