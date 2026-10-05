#include <reliefos/gui.h>
#include <reliefos/syscall.h>
#include <reliefos/ui.h>
#include <stdint.h>
#include <stdlib.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include <reliefos/layout.h>

#define DOOM_KEY_QUEUE_CAP 64U
#define DOOM_WINDOW_WIDTH DOOMGENERIC_RESX
#define DOOM_WINDOW_HEIGHT DOOMGENERIC_RESY

struct doom_key_event {
    int pressed;
    unsigned char key;
};

static struct doom_key_event key_queue[DOOM_KEY_QUEUE_CAP];
static uint32_t key_read;
static uint32_t key_write;
static uint32_t window_id;
static uint8_t headless_mode;
static uint32_t frame[DOOM_WINDOW_WIDTH * DOOM_WINDOW_HEIGHT];
static struct reliefos_ui_surface ui;

static void restore_mouse(void)
{
    if (window_id) {
        reliefos_gui_set_mouse_visible(window_id, 1);
    }
}

static unsigned char doom_key(uint8_t keycode)
{
    switch (keycode) {
    case 1: return KEY_ESCAPE;
    case 14: return KEY_BACKSPACE;
    case 15: return KEY_TAB;
    case 28: return KEY_ENTER;
    case 29: return KEY_FIRE;
    case 42:
    case 54: return KEY_RSHIFT;
    case 56:
    case 115: return KEY_RALT;
    case 57: return KEY_USE;
    case 59: return KEY_F1;
    case 60: return KEY_F2;
    case 61: return KEY_F3;
    case 62: return KEY_F4;
    case 63: return KEY_F5;
    case 64: return KEY_F6;
    case 65: return KEY_F7;
    case 66: return KEY_F8;
    case 67: return KEY_F9;
    case 68: return KEY_F10;
    case 87: return KEY_F11;
    case 88: return KEY_F12;
    case 72:
    case 103: return KEY_UPARROW;
    case 75:
    case 105: return KEY_LEFTARROW;
    case 77:
    case 106: return KEY_RIGHTARROW;
    case 80:
    case 108: return KEY_DOWNARROW;
    case 116: return KEY_RCTRL;
    default:
        break;
    }
    if (keycode >= 2 && keycode <= 11) {
        return (unsigned char)(keycode == 11 ? '0' : '1' + keycode - 2);
    }
    if (keycode == 12) return '-';
    if (keycode == 13) return '=';
    if (keycode >= 16 && keycode <= 25) {
        static const char keys[] = "qwertyuiop";
        return (unsigned char)keys[keycode - 16];
    }
    if (keycode >= 30 && keycode <= 38) {
        static const char keys[] = "asdfghjkl";
        return (unsigned char)keys[keycode - 30];
    }
    if (keycode >= 44 && keycode <= 50) {
        static const char keys[] = "zxcvbnm";
        return (unsigned char)keys[keycode - 44];
    }
    switch (keycode) {
    case 26: return '[';
    case 27: return ']';
    case 39: return ';';
    case 40: return '\'';
    case 41: return '`';
    case 43: return '\\';
    case 51: return ',';
    case 52: return '.';
    case 53: return '/';
    default: return 0;
    }
}

static void queue_key(uint8_t keycode, uint8_t pressed)
{
    unsigned char key = doom_key(keycode);
    uint32_t next;
    if (!key) {
        return;
    }
    next = (key_write + 1U) % DOOM_KEY_QUEUE_CAP;
    if (next == key_read) {
        key_read = (key_read + 1U) % DOOM_KEY_QUEUE_CAP;
    }
    key_queue[key_write].pressed = pressed != 0;
    key_queue[key_write].key = key;
    key_write = next;
}

static void pump_events(void)
{
    struct reliefos_gui_app_event event = {.window_id = window_id};
    while (reliefos_gui_poll_app_event(&event) > 0) {
        if (event.type == RELIEFOS_GUI_APP_EVENT_CLOSE) {
            reliefos_gui_set_mouse_visible(window_id, 1);
            reliefos_gui_destroy_app_window(window_id);
            exit(0);
        }
        if (event.type == RELIEFOS_GUI_APP_EVENT_KEY_DOWN ||
            event.type == RELIEFOS_GUI_APP_EVENT_KEY_UP) {
            queue_key(event.keycode, event.pressed);
        }
        event.window_id = window_id;
    }
}

void DG_Init(void)
{
    uint32_t flags = RELIEFOS_GUI_WINDOW_FULLSCREEN;
    headless_mode = M_CheckParm("-headless") > 0;
    if (headless_mode) {
        /* Keep the normal Doom/WAD/audio initialization while making the
         * runner independent of the framebuffer/Xorg presentation path. */
        I_AtExit(restore_mouse, true);
        return;
    }
    if (M_CheckParm("-windowed") > 0) {
        flags = RELIEFOS_GUI_WINDOW_NO_RESIZE;
    }
    window_id = (uint32_t)reliefos_gui_create_app_window_ex("Doom", "DoomGeneric",
                                                             DOOM_WINDOW_WIDTH,
                                                             DOOM_WINDOW_HEIGHT,
                                                             flags);
    if (!window_id) {
        exit(1);
    }
    if (flags & RELIEFOS_GUI_WINDOW_FULLSCREEN) {
        reliefos_gui_set_mouse_visible(window_id, 0);
    }
    I_AtExit(restore_mouse, true);
}

void DG_StartupProgress(uint32_t progress, const char *message)
{
    if (!window_id) {
        return;
    }
    if (progress > 100U) {
        progress = 100U;
    }
    reliefos_ui_bind(&ui, frame, DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT,
                   DOOM_WINDOW_WIDTH);
    reliefos_ui_rect(&ui, 0, 0, DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT, 0x00101814U);
    reliefos_ui_rect(&ui, 64, 104, DOOM_WINDOW_WIDTH - 128U, 192, 0x001d2c25U);
    reliefos_ui_text(&ui, 96, 140, "DOOM", 0x00f0f5edU, 0x001d2c25U);
    reliefos_ui_text(&ui, 96, 184, message ? message : "Loading", 0x00f0f5edU,
                   0x001d2c25U);
    reliefos_ui_progress(&ui, 96, 224, DOOM_WINDOW_WIDTH - 192U, 20, progress, 100);
    reliefos_gui_present_window(window_id, DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT,
                               DOOM_WINDOW_WIDTH, frame);
    sched_yield();
}

void DG_DrawFrame(void)
{
    if (!window_id || !DG_ScreenBuffer) {
        return;
    }
    reliefos_gui_present_window(window_id, DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT,
                               DOOM_WINDOW_WIDTH, DG_ScreenBuffer);
    pump_events();
}

void DG_SleepMs(uint32_t ms)
{
    /* Game pacing and screen wipes can spend many tics in this callback.
     * Keep the hardware-fed PCM alive while the outer game tick is waiting. */
    I_UpdateSound();
    pump_events();
    sleep_ms(ms);
    I_UpdateSound();
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)reliefos_uptime_ms();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (key_read == key_write) {
        return 0;
    }
    *pressed = key_queue[key_read].pressed;
    *key = key_queue[key_read].key;
    key_read = (key_read + 1U) % DOOM_KEY_QUEUE_CAP;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv, char **envp)
{
    static char *default_argv[] = {
        "doom.elf", "-iwad", RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/freedoom1.wad", 0
    };
    uint32_t headless_deadline;
    (void)envp;
    if (argc <= 1 || !argv || !argv[0]) {
        argc = (int)(sizeof(default_argv) / sizeof(default_argv[0])) - 1;
        argv = default_argv;
    }
    doomgeneric_Create(argc, argv);
    if (headless_mode) {
        uint32_t seconds = 45U;
        int parameter = M_CheckParmWithArgs("-headless-seconds", 1);
        if (parameter > 0) {
            int requested = atoi(myargv[parameter + 1]);
            if (requested > 0 && requested < 3600) {
                seconds = (uint32_t)requested;
            }
        }
        headless_deadline = DG_GetTicksMs() + seconds * 1000U;
    } else {
        headless_deadline = 0U;
    }
    for (;;) {
        doomgeneric_Tick();
        if (headless_deadline &&
            (int32_t)(DG_GetTicksMs() - headless_deadline) >= 0) {
            printf("[doom] headless demo timeout\n");
            I_Quit();
        }
    }
    reliefos_gui_set_mouse_visible(window_id, 1);
    reliefos_gui_destroy_app_window(window_id);
    return 0;
}
