#include "window.h"
#include <FL/Fl_Choice.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <unistd.h>
namespace nc {
namespace {
void clear_secret(Fl_Secret_Input &field) {
  volatile char *s = const_cast<char *>(field.value());
  size_t length = strlen(field.value());
  for (size_t n = 0; n < length; ++n)
    s[n] = 0;
  field.value("");
}
struct Login {
  bool attempt = false, cancel = false;
  static void accept(Fl_Widget *, void *p) {
    static_cast<Login *>(p)->attempt = true;
  }
  static void reject(Fl_Widget *, void *p) {
    static_cast<Login *>(p)->cancel = true;
  }
};
} // namespace
bool authenticate(Session &session) {
  if (geteuid() == 0) {
    if (!session.launch(Auth::Direct)) {
      fl_alert("%s", T("Cannot start the network worker."));
      return false;
    }
    while (session.alive() && !session.ready() && session.problem.empty()) {
      session.pump();
      Fl::wait(.02);
    }
    if (!session.ready()) {
      fl_alert("%s", T("Cannot start the network worker."));
      return false;
    }
    return true;
  }
  Fl_Double_Window window(540, 280, T("Network authentication"));
  Login state;
  Fl_Box info(
      20, 16, 500, 46,
      T("Administrator permission is required to manage network settings."));
  info.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  Fl_Choice method(142, 76, 378, 30, T("Authenticate with"));
  method.add(T("sudo: current user password"));
  method.add(T("root account password"));
  method.value(0);
  Fl_Secret_Input password(142, 121, 378, 32, T("Password"));
  password.maximum_size(1024);
  Fl_Box status(20, 165, 500, 48);
  status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  Fl_Button cancel(302, 230, 102, 32, T("Cancel"));
  cancel.callback(Login::reject, &state);
  Fl_Return_Button enter(416, 230, 104, 32, T("Unlock"));
  enter.callback(Login::accept, &state);
  window.callback(Login::reject, &state);
  window.end();
  window.set_modal();
  reliefos_fltk::style(&window);
  window.show();
  Fl::disable_im(); // Never pass password keystrokes to an input method
                    // provider.
  password.take_focus();
  bool active = false;
  while (window.shown() && !state.cancel) {
    Fl::wait(.025);
    session.pump();
    if (session.ready()) {
      clear_secret(password);
      Fl::enable_im();
      window.hide();
      return true;
    }
    if (active && !session.alive()) {
      active = false;
      method.activate();
      password.activate();
      enter.activate();
      enter.label(T("Retry"));
      status.copy_label(T("Authentication failed. Check the password or choose "
                          "another method."));
      password.take_focus();
    }
    if (active && session.password_requested()) {
      // sudo can request another password without exiting after a failure.
      active = false;
      password.activate();
      enter.activate();
      enter.label(T("Retry"));
      status.copy_label(T("Enter your password to continue authentication."));
      password.take_focus();
    }
    if (state.attempt) {
      state.attempt = false;
      if (!session.alive() &&
          !session.launch(method.value() == 0 ? Auth::Sudo : Auth::Root)) {
        status.copy_label(T("Cannot start the authentication program."));
        clear_secret(password);
        continue;
      }
      if (!session.password(password.value())) {
        status.copy_label(T("Invalid password input."));
        clear_secret(password);
        continue;
      }
      clear_secret(password);
      active = true;
      method.deactivate();
      password.deactivate();
      enter.deactivate();
      status.copy_label(T("Authenticating…"));
    }
  }
  clear_secret(password);
  Fl::enable_im();
  session.stop();
  window.hide();
  return false;
}
} // namespace nc
