#include "../fltk_support.h"
#include "../fltk_pixels.h"
#include <FL/Fl_Double_Window.H>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include <reliefos/syscall.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"
}

struct doom_key_event { int pressed; unsigned char key; };
static doom_key_event key_queue[64];
static unsigned key_read, key_write;
static bool key_down[256];
static bool headless_mode;
static const uint32_t *visible_pixels;
static uint32_t loading_frame[DOOMGENERIC_RESX * DOOMGENERIC_RESY];
static char loading_message[256];
static bool loading;
static bool frame_ready;
static char headless_option[] = "-headless";
static char windowed_option[] = "-windowed";
static char duration_option[] = "-headless-seconds";

static unsigned char doom_key(int key)
{
    switch (key) {
    case FL_Enter: case FL_KP_Enter: return KEY_ENTER;
    case FL_Escape: return KEY_ESCAPE;
    case FL_BackSpace: return KEY_BACKSPACE;
    case FL_Tab: return KEY_TAB;
    case FL_Left: return KEY_LEFTARROW;
    case FL_Right: return KEY_RIGHTARROW;
    case FL_Up: return KEY_UPARROW;
    case FL_Down: return KEY_DOWNARROW;
    case FL_Control_L: case FL_Control_R: return KEY_FIRE;
    case ' ': return KEY_USE;
    case FL_Shift_L: case FL_Shift_R: return KEY_RSHIFT;
    case FL_Alt_L: case FL_Alt_R: return KEY_RALT;
    case FL_F + 1: return KEY_F1;
    case FL_F + 2: return KEY_F2;
    case FL_F + 3: return KEY_F3;
    case FL_F + 4: return KEY_F4;
    case FL_F + 5: return KEY_F5;
    case FL_F + 6: return KEY_F6;
    case FL_F + 7: return KEY_F7;
    case FL_F + 8: return KEY_F8;
    case FL_F + 9: return KEY_F9;
    case FL_F + 10: return KEY_F10;
    case FL_F + 11: return KEY_F11;
    case FL_F + 12: return KEY_F12;
    default: return key > 0 && key < 128 ? static_cast<unsigned char>(tolower(key)) : 0;
    }
}

static void queue_key(unsigned char key, bool pressed)
{
    if (!key) return;
    unsigned next = (key_write + 1) % 64;
    if (next == key_read) key_read = (key_read + 1) % 64;
    key_queue[key_write] = {pressed, key}; key_write = next; key_down[key] = pressed;
}

class DoomWindow : public Fl_Double_Window {
public:
    DoomWindow() : Fl_Double_Window(DOOMGENERIC_RESX, DOOMGENERIC_RESY, "Doom") {}
    void draw() override
    {
        fl_color(FL_BLACK); fl_rectf(0, 0, w(), h());
        unsigned width = w(), height = (uint64_t)width * DOOMGENERIC_RESY / DOOMGENERIC_RESX;
        if (height > static_cast<unsigned>(h())) { height = h(); width = (uint64_t)height * DOOMGENERIC_RESX / DOOMGENERIC_RESY; }
        reliefos_fltk::draw_pixels(visible_pixels, DOOMGENERIC_RESX, DOOMGENERIC_RESY,
            (w()-width)/2, (h()-height)/2, width, height);
        if (loading) {
            fl_font(FL_HELVETICA_BOLD, 24); fl_color(FL_WHITE);
            fl_draw("DOOM", 0, h()/3, w(), 32, FL_ALIGN_CENTER);
            fl_font(FL_HELVETICA, 15);
            fl_draw(loading_message, 20, h()/3+40, w()-40, 50, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
        }
    }
    int handle(int event) override
    {
        if (event == FL_KEYDOWN || event == FL_KEYUP) { queue_key(doom_key(Fl::event_key()), event == FL_KEYDOWN); return 1; }
        if (event == FL_UNFOCUS) {
            for (unsigned i = 1; i < 256; ++i) if (key_down[i]) queue_key(i, false);
            return 1;
        }
        if (event == FL_FOCUS) return 1;
        return Fl_Double_Window::handle(event);
    }
};
static DoomWindow *window;

static void pump_events() { if (window) Fl::check(); }
extern "C" void doom_pump_events() { pump_events(); }
static void present(const uint32_t *pixels)
{
    visible_pixels = pixels;
    if (window) { window->redraw(); Fl::check(); Fl::flush(); }
}

extern "C" void DG_Init()
{
    headless_mode = M_CheckParm(headless_option) > 0;
    if (headless_mode) return;
    reliefos_fltk::init();
    window = new DoomWindow;
    window->callback([](Fl_Widget *, void *) { I_Quit(); });
    window->resizable(window);
    if (M_CheckParm(windowed_option) <= 0) window->fullscreen();
    window->show(); window->cursor(FL_CURSOR_NONE);
}

extern "C" void DG_StartupProgress(uint32_t progress, const char *message)
{
    if (!window) return;
    if (progress > 100) progress = 100;
    for (unsigned i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; ++i) loading_frame[i] = 0x101814;
    for (unsigned y = 224; y < 244; ++y) for (unsigned x = 96; x < DOOMGENERIC_RESX-96; ++x)
        loading_frame[y * DOOMGENERIC_RESX + x] = x < 96 + (DOOMGENERIC_RESX-192)*progress/100 ? 0x7ab648 : 0x2a3f33;
    snprintf(loading_message, sizeof(loading_message), "%s", message ? message : "");
    loading = true; present(loading_frame);
}
extern "C" void DG_DrawFrame()
{
    loading = false;
    pump_events();
    present(DG_ScreenBuffer);
    if (window && !frame_ready) {
        frame_ready = true;
        puts("[doom.elf] FLTK game frame ready");
        fflush(stdout);
    }
}
extern "C" int DG_GetKey(int *pressed, unsigned char *key)
{
    if (key_read == key_write) return 0;
    *pressed = key_queue[key_read].pressed; *key = key_queue[key_read].key;
    key_read = (key_read + 1) % 64; return 1;
}
extern "C" void DG_SetWindowTitle(const char *title) { if (window && title) window->copy_label(title); }

int main(int argc, char **argv)
{
    static char arg0[] = "doom.elf", iwad_flag[] = "-iwad";
    static char iwad[] = RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/freedoom1.wad";
    static char *defaults[] = {arg0, iwad_flag, iwad, nullptr};
    if (argc <= 1) { argc = 3; argv = defaults; }
    doomgeneric_Create(argc, argv);
    uint32_t deadline = 0;
    if (headless_mode) {
        unsigned seconds = 45;
        int parameter = M_CheckParmWithArgs(duration_option, 1);
        if (parameter > 0) { int value = atoi(myargv[parameter+1]); if (value > 0 && value < 3600) seconds = value; }
        deadline = DG_GetTicksMs() + seconds * 1000;
    }
    for (;;) {
        doomgeneric_Tick();
        if (deadline && static_cast<int32_t>(DG_GetTicksMs()-deadline) >= 0) {
            printf("[doom] headless demo timeout\n"); I_Quit();
        }
    }
}
