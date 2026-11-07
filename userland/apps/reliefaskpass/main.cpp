/* Unprivileged sudo askpass helper. The password travels only on stdout. */
#include "../fltk_support.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "password_io.h"

static bool accepted;

static void accept(Fl_Widget *widget, void *)
{
    accepted = true;
    widget->window()->hide();
}

static void cancel(Fl_Widget *widget, void *)
{
    if (widget->window()) widget->window()->hide();
    else widget->hide();
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    reliefos_fltk::init();
    Fl_Double_Window window(480, 182, T("Authentication"));
    window.callback(cancel);
    Fl_Box prompt(18, 14, 444, 48);
    prompt.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    prompt.copy_label(argv[1]);
    Fl_Secret_Input password(18, 72, 444, 30);
    password.maximum_size(255);
    Fl_Button cancel_button(252, 130, 100, 30, T("Cancel"));
    cancel_button.callback(cancel);
    Fl_Return_Button ok(362, 130, 100, 30, T("OK"));
    ok.callback(accept);
    window.end();
    reliefos_fltk::style(&window);
    window.set_modal();
    window.show();
    password.take_focus();
    Fl::run();
    int result = accepted ? askpass_write_password(password.value()) : 1;
    explicit_bzero(const_cast<char *>(password.value()), strlen(password.value()));
    password.value("");
    return result;
}
