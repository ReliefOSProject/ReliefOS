#include <reliefos/gui.h>
#include <reliefos/layout.h>
#include <reliefos/syscall.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"

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
static Display *display;
static Window window;
static GC gc;
static Atom wm_delete_window;
static uint8_t headless_mode;
static uint32_t frame[DOOM_WINDOW_WIDTH * DOOM_WINDOW_HEIGHT];

static unsigned char doom_key(KeySym symbol)
{
    switch (symbol) {
    case XK_Return: return KEY_ENTER;
    case XK_Escape: return KEY_ESCAPE;
    case XK_BackSpace: return KEY_BACKSPACE;
    case XK_Tab: return KEY_TAB;
    case XK_Left: return KEY_LEFTARROW;
    case XK_Right: return KEY_RIGHTARROW;
    case XK_Up: return KEY_UPARROW;
    case XK_Down: return KEY_DOWNARROW;
    case XK_Control_L:
    case XK_Control_R: return KEY_FIRE;
    case XK_space: return KEY_USE;
    case XK_Shift_L:
    case XK_Shift_R: return KEY_RSHIFT;
    case XK_Alt_L:
    case XK_Alt_R: return KEY_RALT;
    case XK_F1: return KEY_F1;
    case XK_F2: return KEY_F2;
    case XK_F3: return KEY_F3;
    case XK_F4: return KEY_F4;
    case XK_F5: return KEY_F5;
    case XK_F6: return KEY_F6;
    case XK_F7: return KEY_F7;
    case XK_F8: return KEY_F8;
    case XK_F9: return KEY_F9;
    case XK_F10: return KEY_F10;
    case XK_F11: return KEY_F11;
    case XK_F12: return KEY_F12;
    default: break;
    }
    /* Letters, digits and US-layout punctuation share their ASCII codes with
     * their KeySym values, so the doomgeneric convention applies. */
    if (symbol > 0 && symbol < 128) {
        return (unsigned char)tolower((int)symbol);
    }
    return 0;
}

static void queue_key(KeySym symbol, uint8_t pressed)
{
    unsigned char key = doom_key(symbol);
    uint32_t next;
    if (!key) return;
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
    if (!display) return;
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == KeyPress) {
            queue_key(XkbKeycodeToKeysym(display, event.xkey.keycode, 0, 0), 1);
        } else if (event.type == KeyRelease) {
            queue_key(XkbKeycodeToKeysym(display, event.xkey.keycode, 0, 0), 0);
        } else if (event.type == ClientMessage &&
                   (Atom)event.xclient.data.l[0] == wm_delete_window) {
            I_Quit();
        }
    }
}

/* Wrap the 0x00RRGGBB buffer in a temporary XImage; the pixels belong to the
 * caller, so detach before freeing the wrapper. */
static void present(const uint32_t *buffer)
{
    XImage *image;
    if (!display || !buffer) return;
    image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                         DefaultDepth(display, DefaultScreen(display)),
                         ZPixmap, 0, (char *)buffer,
                         DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT, 32, 0);
    if (!image) return;
    XPutImage(display, window, gc, image, 0, 0, 0, 0,
              DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT);
    image->data = NULL;
    XDestroyImage(image);
    XFlush(display);
}

void DG_Init(void)
{
    XColor black = {0};
    Pixmap blank;
    Cursor hidden;
    headless_mode = M_CheckParm("-headless") > 0;
    if (headless_mode) {
        /* Keep the normal Doom/WAD/audio initialization while making the
         * runner independent of the X presentation path. */
        return;
    }
    display = XOpenDisplay(NULL);
    if (!display) {
        exit(1);
    }
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0,
                                 DOOM_WINDOW_WIDTH, DOOM_WINDOW_HEIGHT,
                                 0, BlackPixel(display, DefaultScreen(display)),
                                 BlackPixel(display, DefaultScreen(display)));
    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | KeyReleaseMask |
                 StructureNotifyMask);
    if (M_CheckParm("-windowed") <= 0) {
        /* EWMH fullscreen; IceWM honors the request before mapping. */
        Atom state = XInternAtom(display, "_NET_WM_STATE", False);
        Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(display, window, state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&fullscreen, 1);
    }
    wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete_window, 1);
    XStoreName(display, window, "Doom");
    XMapWindow(display, window);
    gc = XCreateGC(display, window, 0, NULL);
    XkbSetDetectableAutoRepeat(display, 1, 0);
    blank = XCreateBitmapFromData(display, window, (char *)"\0", 1, 1);
    hidden = XCreatePixmapCursor(display, blank, blank, &black, &black, 0, 0);
    XDefineCursor(display, window, hidden);
    XFreePixmap(display, blank);
    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == MapNotify) break;
    }
}

void DG_StartupProgress(uint32_t progress, const char *message)
{
    if (!display) return;
    if (progress > 100U) {
        progress = 100U;
    }
    for (uint32_t i = 0; i < DOOM_WINDOW_WIDTH * DOOM_WINDOW_HEIGHT; ++i) {
        frame[i] = 0x00101814U;
    }
    for (uint32_t y = 104; y < 296; ++y) {
        for (uint32_t x = 64; x < DOOM_WINDOW_WIDTH - 64U; ++x) {
            frame[y * DOOM_WINDOW_WIDTH + x] = 0x001d2c25U;
        }
    }
    for (uint32_t y = 224; y < 244; ++y) {
        for (uint32_t x = 96; x < DOOM_WINDOW_WIDTH - 96U; ++x) {
            uint32_t filled = 96U + (uint32_t)((uint64_t)(DOOM_WINDOW_WIDTH - 192U) *
                                               progress / 100U);
            frame[y * DOOM_WINDOW_WIDTH + x] =
                x < filled ? 0x007ab648U : 0x002a3f33U;
        }
    }
    present(frame);
    XSetForeground(display, gc, 0x00f0f5edU);
    XDrawString(display, window, gc, 96, 150, "DOOM", 4);
    if (message) {
        XDrawString(display, window, gc, 96, 190, message, (int)strlen(message));
    }
    XFlush(display);
}

void DG_DrawFrame(void)
{
    pump_events();
    present(DG_ScreenBuffer);
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
    if (display && window && title) {
        XStoreName(display, window, title);
    }
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
    return 0;
}
