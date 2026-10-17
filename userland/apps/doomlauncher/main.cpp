#include "../fltk_support.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Box.H>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
extern "C" {
#include "model.h"
#include <reliefos/launch.h>
#include <reliefos/launch_result.h>
}

#define DOOM_PATH RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/doom.elf"
#define DEFAULT_IWAD RELIEFOS_LAYOUT_RELIEFOS_APPS "/doom/freedoom1.wad"
static Fl_Input *iwad, *arguments;
static Fl_Check_Button *silent, *fullscreen;
static Fl_Button *launch_button;
static Fl_Box *status;
static pid_t child;

static void reset(Fl_Widget *, void *)
{
    doomlauncher_options options{};
    doomlauncher_defaults(&options, DEFAULT_IWAD);
    iwad->value(options.iwad); arguments->value(options.extra);
    silent->value(options.disable_sound); fullscreen->value(options.fullscreen);
    status->copy_label(T("Settings reset"));
}

static const char *argument_error(int error)
{
    switch (error) {
    case LAUNCH_RESULT_EMPTY: return T("Arguments are empty");
    case LAUNCH_RESULT_TOO_MANY_ARGS: return T("Too many arguments");
    case LAUNCH_RESULT_UNCLOSED_QUOTE: return T("Missing closing quote");
    default: return reliefos_launch_error_text(error);
    }
}

static void launch(Fl_Widget *, void *)
{
    if (child) return;
    doomlauncher_options options{};
    if (!reliefos_fltk::copy_input(options.iwad, sizeof(options.iwad), iwad->value()) ||
        !reliefos_fltk::copy_input(options.extra, sizeof(options.extra), arguments->value())) {
        status->copy_label(T("Input is too long")); return;
    }
    options.disable_sound = silent->value(); options.fullscreen = fullscreen->value();
    char extra_copy[DOOMLAUNCHER_EXTRA_CAP];
    reliefos_fltk::copy_input(extra_copy, sizeof(extra_copy), options.extra);
    char *extra[DOOMLAUNCHER_MAX_ARGS], *argv[DOOMLAUNCHER_MAX_ARGS];
    int count = options.extra[0] ? reliefos_cmdline_split(extra_copy, extra, DOOMLAUNCHER_MAX_ARGS) : 0;
    if (count < 0) { status->copy_label(argument_error(count)); return; }
    int built = doomlauncher_build_argv(DOOM_PATH, &options, extra, count, argv, DOOMLAUNCHER_MAX_ARGS);
    if (built < 0) { status->copy_label(built == DOOMLAUNCHER_ERR_NO_IWAD ? T("An IWAD path is required") : T("Too many arguments")); return; }
    int pid = reliefos_spawn_argv(DOOM_PATH, argv);
    if (pid < 0) {
        char message[240]; snprintf(message, sizeof(message), "%s%d", T("Launch failed: "), pid);
        status->copy_label(message); return;
    }
    child = pid; launch_button->deactivate();
    status->copy_label(T("Starting DOOM: the game window shows loading progress"));
}

static void poll(void *)
{
    if (child) {
        int result = 0;
        pid_t reaped = waitpid(child, &result, WNOHANG);
        if (reaped == child) {
            char text[160]; snprintf(text, sizeof(text), "%s%d", T("DOOM exited with code "), doomlauncher_exit_code(reaped, result));
            status->copy_label(text); child = 0; launch_button->activate();
        } else if (reaped < 0 && errno == ECHILD) {
            child = 0; launch_button->activate(); status->copy_label(T("DOOM is no longer running"));
        }
    }
    Fl::repeat_timeout(.25, poll);
}

int main()
{
    reliefos_fltk::init("doomlauncher");
    Fl_Double_Window window(660, 320, T("DOOM Launcher"));
    Fl_Box *label = new Fl_Box(18, 12, 620, 26, T("IWAD path")); label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    iwad = new Fl_Input(18, 42, 620, 34); iwad->textsize(15);
    label = new Fl_Box(18, 88, 620, 26, T("Extra DOOM arguments")); label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    arguments = new Fl_Input(18, 118, 620, 34); arguments->textsize(15);
    iwad->when(FL_WHEN_ENTER_KEY_ALWAYS); iwad->callback(launch);
    arguments->when(FL_WHEN_ENTER_KEY_ALWAYS); arguments->callback(launch);
    silent = new Fl_Check_Button(18, 164, 290, 30, T("Disable sound"));
    fullscreen = new Fl_Check_Button(328, 164, 300, 30, T("Fullscreen"));
    launch_button = new Fl_Button(18, 210, 126, 36, T("Launch")); launch_button->callback(launch);
    Fl_Button *reset_button = new Fl_Button(160, 210, 126, 36, T("Reset")); reset_button->callback(reset);
    status = new Fl_Box(18, 262, 620, 40); status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    reset(nullptr, nullptr); status->copy_label(T("Ready"));
    window.end(); reliefos_fltk::style(&window); window.show(); Fl::add_timeout(.25, poll);
    puts("[doomlauncher.elf] FLTK DOOM launcher ready");
    return Fl::run();
}
