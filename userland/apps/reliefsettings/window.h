#ifndef RELIEFSETTINGS_WINDOW_H
#define RELIEFSETTINGS_WINDOW_H
#include "../fltk_support.h"
#include "backend.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Text_Buffer.H>

namespace rs {
class App {
    Fl_Double_Window *window_;
    Fl_Hold_Browser *navigation_, *zone_list_;
    Fl_Group *pages_[6];
    Fl_Box *status_, *clock_, *preview_text_, *scope_;
    Fl_Choice *language_, *output_, *mode_, *layout_, *variant_;
    Fl_Input *zone_search_, *keyboard_test_;
    Fl_Output *timezone_, *current_mode_, *config_path_;
    Fl_Button *apply_, *refresh_, *network_, *services_;
    Fl_Double_Window *preview_ = nullptr;
    Fl_Double_Window *error_window_ = nullptr;
    Fl_Text_Buffer error_buffer_;
    Fl_Button *keep_ = nullptr, *restore_ = nullptr;
    Text directory_;
    Snapshot locale_snapshot_, desktop_snapshot_;
    Array<Output> outputs_;
    Array<KeyboardLayout> layouts_;
    Array<Text> zones_;
    Job job_;
    ra::Process process_;
    enum { Idle, QueryDisplay, Timezone } process_kind_ = Idle;
    enum { Nothing, Language, Keyboard, Display } job_kind_ = Nothing;
    bool config_ok_ = false, locale_ok_ = false;
    double countdown_ = 0, operation_started_ = 0;
    int last_second_ = -1;
    pid_t launched_[2] = {-1, -1};
    static void tick(void *);
    static void navigate(Fl_Widget *, void *);
    static void apply_callback(Fl_Widget *, void *);
    void pump();
    void controls();
    void reload();
    void refresh_display();
    void fill_modes();
    void fill_variants();
    void filter_zones();
    void apply();
    void confirm_display();
    void decide(bool);
    void launch(unsigned);
    void status(const char *);
    void show_error(const char *);
    bool busy() const {
        return job_.active() || process_.running();
    }
    int display_fd() const;

  public:
    App();
    ~App();
    void show();
    void resize();
    void close();
};
} // namespace rs
#endif
