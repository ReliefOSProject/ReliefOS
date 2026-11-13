#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/launch.h>
#include <reliefos/launch_result.h>
#include <reliefos/psf_font.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <reliefos/layout.h>

#define RUN_W 360
#define RUN_H 148
#define PATH_MAX_LEN RELIEFOS_FS_PATH_LEN
#define T(s) gettext(s)

static uint32_t pixels[RUN_W * RUN_H];
static char input_path[PATH_MAX_LEN] = RELIEFOS_LAYOUT_RELIEFOS_APPS "/";
static char status_text[96];
static struct reliefos_ui_edit_state input_edit;

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

static void append_text(char *dst, uint32_t cap, const char *prefix, int value)
{
    uint32_t pos = 0;
    copy_text(dst, cap, "");
    while (prefix && *prefix && pos + 1 < cap) {
        dst[pos++] = *prefix++;
    }
    if (value < 0 && pos + 1 < cap) {
        dst[pos++] = '-';
        value = -value;
    }
    if (value == 0) {
        if (pos + 1 < cap) {
            dst[pos++] = '0';
        }
    } else {
        char tmp[16];
        uint32_t n = 0;
        while (value > 0 && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (n && pos + 1 < cap) {
            dst[pos++] = tmp[--n];
        }
    }
    dst[pos] = 0;
}

static void draw_run(struct reliefos_ui_surface *ui)
{
    reliefos_ui_rect(ui, 0, 0, RUN_W, RUN_H, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 12, 14, T("Open ReliefOS program or file path"), RELIEFOS_UI_BLACK, RELIEFOS_UI_GRAY);
    reliefos_ui_text(ui, 12, 38, T("Path:"), RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_edit_state_draw(ui, 56, 34, RUN_W - 68, &input_edit, 0);
    reliefos_ui_statusbar(ui, RUN_H - 28, 28, status_text);
}

static void launch_path(int window_id)
{
    char *argv[RELIEFOS_LAUNCH_MAX_ARGS + 1];
    int pid;
    pid = reliefos_launch_command_line(input_path, argv, RELIEFOS_LAUNCH_MAX_ARGS + 1);
    if (pid < 0) {
        if (reliefos_launch_is_error(pid)) {
            copy_text(status_text, sizeof(status_text), reliefos_launch_error_text(pid));
        } else {
            append_text(status_text, sizeof(status_text), T("Launch failed "), pid);
        }
        return;
    }
    printf("[run.elf] launch command=%s pid=%d\n", input_path, pid);
    (void)window_id;
    exit(0);
}

int main(int argc, char **argv, char **envp)
{
    setlocale(LC_ALL, "");
    bindtextdomain("run", RELIEFOS_LAYOUT_LOCALE);
    textdomain("run");
    struct reliefos_ui_surface ui;
    struct reliefos_gui_app_event event;
    int window_id;
    (void)argc;
    (void)argv;
    (void)envp;

    puts("[run.elf] run dialog starting");
    copy_text(status_text, sizeof(status_text), T("Enter a file path and press Enter"));
    window_id = reliefos_gui_create_app_window_ex(T("Run"), T("Open file path"),
                                                RUN_W, RUN_H, RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        printf("[run.elf] create window failed=%d\n", window_id);
        return 1;
    }

    reliefos_ui_bind(&ui, pixels, RUN_W, RUN_H, RUN_W);
    reliefos_ui_edit_state_init(&input_edit, input_path, sizeof(input_path));
    input_edit.focused = 1;
    draw_run(&ui);
    reliefos_gui_present_window((uint32_t)window_id, RUN_W, RUN_H, RUN_W, pixels);

    for (;;) {
        event.window_id = (uint32_t)window_id;
        while (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) > 0) {
            if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
                return 0;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON) {
                if (reliefos_ui_edit_state_handle_mouse(&input_edit, event.x, event.y,
                                                      56, 34, RUN_W - 68, event.buttons)) {
                    draw_run(&ui);
                    reliefos_gui_present_window((uint32_t)window_id, RUN_W, RUN_H, RUN_W, pixels);
                }
                continue;
            }
            if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN || event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
                if (event.pressed && event.keycode == RELIEFOS_KEY_ENTER) {
                    launch_path(window_id);
                } else if (!reliefos_ui_edit_state_handle_key(&input_edit, event.keycode, event.pressed)) {
                    continue;
                }
                draw_run(&ui);
                reliefos_gui_present_window((uint32_t)window_id, RUN_W, RUN_H, RUN_W, pixels);
            }
        }
        sleep_ms(10);
    }
}
