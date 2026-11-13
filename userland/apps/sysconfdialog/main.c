#include <reliefos/auth.h>
#include <reliefos/fs.h>
#include <reliefos/gui.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <reliefos/startup.h>
#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <string.h>

#define DIALOG_W 560U
#define DIALOG_H 300U
#define T(s) gettext(s)

static uint32_t pixels[DIALOG_W * DIALOG_H];

static int hit_rect(int32_t x, int32_t y, uint32_t rx, uint32_t ry,
                    uint32_t rw, uint32_t rh)
{
    return x >= (int32_t)rx && y >= (int32_t)ry &&
           x < (int32_t)(rx + rw) && y < (int32_t)(ry + rh);
}

static void append_char(char *text, uint32_t *pos, uint32_t cap, char ch)
{
    if (*pos + 1U < cap) {
        text[(*pos)++] = ch;
        text[*pos] = 0;
    }
}

static void append_text(char *text, uint32_t *pos, uint32_t cap, const char *value)
{
    while (value && *value) {
        append_char(text, pos, cap, *value++);
    }
}

static void format_args(char *text, uint32_t cap,
                        const struct reliefos_startup_command *command)
{
    uint32_t pos = 0;
    text[0] = 0;
    if (!command->argc) {
        append_text(text, &pos, cap, T("None"));
        return;
    }
    for (uint32_t i = 0; i < command->argc; ++i) {
        if (i) {
            append_char(text, &pos, cap, ' ');
        }
        append_text(text, &pos, cap, command->args[i]);
    }
}

static void draw_dialog(struct reliefos_ui_surface *ui,
                        const struct reliefos_startup_dialog_request *request,
                        uint8_t remember)
{
    char args[RELIEFOS_STARTUP_MAX_ARGS * (RELIEFOS_STARTUP_ARG_LEN + 1U) + 8U];
    reliefos_ui_rect(ui, 0, 0, DIALOG_W, DIALOG_H, RELIEFOS_UI_GRAY);
    reliefos_ui_panel(ui, 16, 16, DIALOG_W - 32U, DIALOG_H - 32U, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 36,
                   T("Allow startup application?"),
                   RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 32, 68, DIALOG_W - 64U,
                           T("Allow this app to start a process when you sign in?"),
                           RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 106, T("Requesting application"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 32, 126, DIALOG_W - 64U, request->requester_path,
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    reliefos_ui_text(ui, 32, 158, T("Startup command"),
                   RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_text_clipped(ui, 32, 178, DIALOG_W - 64U, request->command.path,
                           RELIEFOS_UI_BLACK, RELIEFOS_UI_WHITE);
    format_args(args, sizeof(args), &request->command);
    reliefos_ui_text_clipped(ui, 32, 202, DIALOG_W - 64U, args,
                           RELIEFOS_UI_DARK, RELIEFOS_UI_WHITE);
    reliefos_ui_checkbox(ui, 32, 224,
                       T("Do not ask again if I deny this request"),
                       remember, 0);
    reliefos_ui_button(ui, DIALOG_W - 196U, DIALOG_H - 52U, 76U,
                     RELIEFOS_UI_BUTTON_H, T("Deny"), 0);
    reliefos_ui_button(ui, DIALOG_W - 108U, DIALOG_H - 52U, 76U,
                     RELIEFOS_UI_BUTTON_H, T("Allow"), 0);
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain("sysconfdialog", RELIEFOS_LAYOUT_LOCALE);
    textdomain("sysconfdialog");
    struct reliefos_startup_dialog_request request;
    struct reliefos_gui_app_event event;
    struct reliefos_ui_surface ui;
    uint8_t remember = 0;
    int window_id;

    (void)argc;
    (void)argv;
    if (reliefos_startup_dialog_get(&request) < 0) {
        return 1;
    }
    window_id = reliefos_gui_create_app_window_ex(T("Startup Application"),
                                                T("Startup permission"),
                                                DIALOG_W, DIALOG_H,
                                                RELIEFOS_GUI_WINDOW_NO_RESIZE);
    if (window_id <= 0) {
        (void)reliefos_startup_dialog_resolve(request.request_id,
                                            RELIEFOS_STARTUP_DECISION_DENY);
        return 1;
    }
    reliefos_ui_bind(&ui, pixels, DIALOG_W, DIALOG_H, DIALOG_W);
    for (;;) {
        draw_dialog(&ui, &request, remember);
        reliefos_gui_present_window((uint32_t)window_id, DIALOG_W, DIALOG_H,
                                  DIALOG_W, pixels);
        event.window_id = (uint32_t)window_id;
        if (reliefos_gui_wait_app_event(&event, RELIEFOS_GUI_IDLE_WAIT_MS) <= 0) {
            sleep_ms(10);
            continue;
        }
        if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE ||
            (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event.keycode == 1U)) {
            (void)reliefos_startup_dialog_resolve(request.request_id,
                                                RELIEFOS_STARTUP_DECISION_DENY);
            break;
        }
        if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN && event.keycode == RELIEFOS_KEY_ENTER) {
            (void)reliefos_startup_dialog_resolve(request.request_id,
                                                RELIEFOS_STARTUP_DECISION_ALLOW);
            break;
        }
        if (event.type == RELIEFOS_GUI_APP_EVENT_MOUSE_BUTTON && (event.buttons & 1U)) {
            if (hit_rect(event.x, event.y, 32, 218, 330, RELIEFOS_UI_BUTTON_H)) {
                remember = remember ? 0U : 1U;
            } else if (hit_rect(event.x, event.y, DIALOG_W - 196U, DIALOG_H - 52U,
                                76U, RELIEFOS_UI_BUTTON_H)) {
                (void)reliefos_startup_dialog_resolve(request.request_id,
                                                    remember ? RELIEFOS_STARTUP_DECISION_DENY_REMEMBERED
                                                             : RELIEFOS_STARTUP_DECISION_DENY);
                break;
            } else if (hit_rect(event.x, event.y, DIALOG_W - 108U, DIALOG_H - 52U,
                                76U, RELIEFOS_UI_BUTTON_H)) {
                (void)reliefos_startup_dialog_resolve(request.request_id,
                                                    RELIEFOS_STARTUP_DECISION_ALLOW);
                break;
            }
        }
    }
    reliefos_gui_destroy_app_window((uint32_t)window_id);
    return 0;
}
