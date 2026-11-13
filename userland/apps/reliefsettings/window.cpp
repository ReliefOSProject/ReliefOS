#include "window.h"
#include "../locale_settings.h"
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>
#include <FL/x.H>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace rs {
namespace {
class Window : public Fl_Double_Window {
    App &app_;

  public:
    explicit Window(App &app) : Fl_Double_Window(900, 600, "ReliefSettings"), app_(app) {}
    void resize(int x, int y, int w, int h) override {
        Fl_Double_Window::resize(x, y, w, h);
        app_.resize();
    }
};
Fl_Box *note(int y, int h, const char *text) {
    auto *box = new Fl_Box(220, y, 650, h, text);
    box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_CLIP);
    return box;
}
void menu_item(Fl_Choice *choice, const char *text) {
    int index = choice->add(" ");
    choice->replace(index, text);
}
bool common_layouts_first(Array<KeyboardLayout> &layouts) {
    Array<KeyboardLayout> ordered;
    if (!ordered.reserve(layouts.size()))
        return false;
    const char *common[] = {"us", "gb", "de", "fr", "cn", "jp", "ru"};
    for (const char *id : common)
        for (size_t i = 0; i < layouts.size(); ++i)
            if (layouts[i].id == id)
                ordered.push(static_cast<KeyboardLayout &&>(layouts[i]));
    for (size_t i = 0; i < layouts.size(); ++i)
        if (!layouts[i].id.empty())
            ordered.push(static_cast<KeyboardLayout &&>(layouts[i]));
    layouts = static_cast<Array<KeyboardLayout> &&>(ordered);
    return true;
}
template <class Widget> Widget *field(int y, const char *label) {
    auto *widget = new Widget(220, y, 650, 32, label);
    widget->align(FL_ALIGN_TOP_LEFT);
    return widget;
}
const char *page_names[] = {"Language", "Date and time", "Display",
                            "Keyboard", "Network",       "Services"};
constexpr int language_count = sizeof(language_options) / sizeof(language_options[0]);
} // namespace
App::App() {
    Text error;
    config_directory(directory_, error);
    window_ = new Window(*this);
    window_->xclass("reliefos-reliefsettings");
    window_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->close(); }, this);
    navigation_ = new Fl_Hold_Browser(12, 14, 180, 572);
    navigation_->textsize(16);
    for (const char *name : page_names)
        navigation_->add(T(name));
    navigation_->value(1);
    navigation_->callback(navigate, this);

    pages_[0] = new Fl_Group(208, 14, 680, 490);
    language_ = field<Fl_Choice>(64, T("Application language"));
    language_->add(T("System default"));
    for (const auto &option : language_options)
        menu_item(language_, option.name);
    language_->value(0);
    note(114, 58,
         T("The language is used the next time you sign in. Save your work before signing out."));
    config_path_ = field<Fl_Output>(220, T("User configuration directory"));
    config_path_->value(directory_.c_str());
    note(274, 58,
         T("Your choice applies to this account. Other users keep their own language settings."));
    pages_[0]->end();

    pages_[1] = new Fl_Group(208, 14, 680, 490);
    timezone_ = field<Fl_Output>(58, T("System time zone"));
    clock_ = note(98, 34, "");
    zone_search_ = field<Fl_Input>(176, T("Find a time zone"));
    zone_search_->when(FL_WHEN_CHANGED);
    zone_search_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->filter_zones(); },
                           this);
    zone_list_ = new Fl_Hold_Browser(220, 222, 650, 198);
    zone_list_->textsize(15);
    note(432, 54,
         T("Time zone changes apply system-wide. Administrator authentication is requested when "
           "needed."));
    pages_[1]->end();

    pages_[2] = new Fl_Group(208, 14, 680, 490);
    output_ = field<Fl_Choice>(60, T("Display output"));
    output_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->fill_modes(); }, this);
    current_mode_ = field<Fl_Output>(140, T("Current mode"));
    mode_ = field<Fl_Choice>(220, T("Resolution and refresh rate"));
    note(278, 74,
         T("After applying, choose Keep within 15 seconds. The previous mode is restored "
           "automatically if you do not confirm."));
    note(362, 60,
         T("Available modes are reported by the X server and graphics driver. Confirmed settings "
           "are restored at your next sign-in."));
    pages_[2]->end();

    pages_[3] = new Fl_Group(208, 14, 680, 490);
    layout_ = field<Fl_Choice>(64, T("Keyboard layout"));
    layout_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->fill_variants(); }, this);
    variant_ = field<Fl_Choice>(144, T("Layout variant"));
    keyboard_test_ = field<Fl_Input>(248, T("Test your keyboard here"));
    note(302, 66,
         T("Apply a layout, then type in the test field. Chinese text input is provided by your "
           "input method."));
    note(380, 66,
         T("System default clears your saved override and takes effect at your next sign-in."));
    pages_[3]->end();

    pages_[4] = new Fl_Group(208, 14, 680, 490);
    network_ = new Fl_Button(220, 64, 650, 38, T("Open Network Controller"));
    network_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->launch(0); }, this);
    note(126, 64,
         T("Manage network adapters, automatic or static addressing, DNS and connection "
           "diagnostics."));
    pages_[4]->end();

    pages_[5] = new Fl_Group(208, 14, 680, 490);
    services_ = new Fl_Button(220, 64, 650, 38, T("Open OpenRC Controller"));
    services_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->launch(1); }, this);
    note(126, 64, T("Inspect services, start or stop them and manage their startup runlevels."));
    pages_[5]->end();
    for (unsigned i = 0; i < 6; ++i) {
        pages_[i]->resizable(nullptr);
        if (i)
            pages_[i]->hide();
    }

    scope_ = note(494, 28, T("Current user"));
    refresh_ = new Fl_Button(220, 546, 118, 34, T("Reload"));
    refresh_->callback(
        [](Fl_Widget *, void *p) {
            auto *app = static_cast<App *>(p);
            app->reload();
            app->refresh_display();
        },
        this);
    apply_ = new Fl_Button(742, 546, 128, 34, T("Save"));
    apply_->callback(apply_callback, this);
    status_ = new Fl_Box(350, 534, 380, 54);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_CLIP);
    window_->end();
    window_->size_range(800, 560);
    reliefos_fltk::style(window_);

    Text rules;
    if (read_text("/usr/share/X11/xkb/rules/evdev.lst", rules, error))
        parse_layouts(rules.c_str(), layouts_, error);
    common_layouts_first(layouts_);
    layout_->add(T("System default"));
    for (size_t i = 0; i < layouts_.size(); ++i) {
        // XKB descriptions can contain slashes, which add() treats as menus.
        Text label = layouts_[i].id;
        if (!append(label, " — ") || !append(label, layouts_[i].label.c_str()))
            label = layouts_[i].id;
        menu_item(layout_, label.c_str());
    }
    layout_->value(0);
    load_zones(zones_, error);
    filter_zones();
    reload();
    refresh_display();
    resize();
    Fl::add_timeout(.05, tick, this);
}
App::~App() {
    Fl::remove_timeout(tick, this);
    if (preview_)
        delete preview_;
    if (error_window_)
        delete error_window_;
    delete window_;
}
void App::show() {
    window_->show();
    fprintf(stderr, "[reliefsettings] FLTK settings center ready\n");
}
int App::display_fd() const {
    return fl_display ? ConnectionNumber(fl_display) : -1;
}
void App::status(const char *text) {
    status_->copy_label(text);
}
void App::show_error(const char *details) {
    if (!error_window_) {
        error_window_ = new Fl_Double_Window(650, 380, T("Settings diagnostic"));
        auto *text = new Fl_Text_Display(16, 14, 618, 298);
        text->buffer(error_buffer_);
        text->textsize(15);
        text->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        auto *close = new Fl_Button(526, 330, 108, 34, T("Close"));
        close->callback([](Fl_Widget *, void *p) { static_cast<Fl_Window *>(p)->hide(); },
                        error_window_);
        error_window_->callback([](Fl_Widget *w, void *) { w->hide(); });
        error_window_->end();
        error_window_->set_modal();
        reliefos_fltk::style(error_window_);
    }
    error_buffer_.text(T(details));
    error_window_->position((Fl::w() - 650) / 2, (Fl::h() - 380) / 2);
    error_window_->show();
}
void App::resize() {
    int width = window_->w(), height = window_->h();
    navigation_->resize(12, 14, 180, height - 28);
    for (auto *page : pages_) {
        page->resize(208, 14, width - 220, height - 110);
        for (int i = 0; i < page->children(); ++i) {
            auto *widget = page->child(i);
            widget->resize(220, widget->y(), width - 250, widget->h());
        }
    }
    zone_list_->resize(220, 222, width - 250, height - 402);
    pages_[1]->child(pages_[1]->children() - 1)->resize(220, height - 168, width - 250, 54);
    scope_->resize(220, height - 106, width - 250, 28);
    refresh_->resize(220, height - 54, 118, 34);
    apply_->resize(width - 158, height - 54, 128, 34);
    status_->resize(350, height - 66, width - 520, 54);
}
void App::navigate(Fl_Widget *, void *p) {
    auto *app = static_cast<App *>(p);
    unsigned selected = app->navigation_->value() - 1;
    if (selected >= 6)
        return;
    for (unsigned i = 0; i < 6; ++i) {
        if (i == selected)
            app->pages_[i]->show();
        else
            app->pages_[i]->hide();
    }
    app->status("");
    app->controls();
    app->window_->redraw();
}
void App::controls() {
    bool active = !busy();
    if (active)
        navigation_->activate();
    else
        navigation_->deactivate();
    for (auto *page : pages_) {
        if (active)
            page->activate();
        else
            page->deactivate();
    }
    if (active)
        refresh_->activate();
    else
        refresh_->deactivate();
    int page = navigation_->value() - 1;
    if (page >= 4)
        apply_->hide();
    else
        apply_->show();
    apply_->label(page == 0 ? T("Save") : T("Apply"));
    scope_->copy_label(page == 1 ? T("All users") : T("Current user"));
    if (page >= 4)
        scope_->hide();
    else
        scope_->show();
    bool valid = page == 0 ? locale_ok_ : config_ok_;
    if (page == 1)
        valid = !zones_.empty();
    if (page == 2)
        valid = valid && mode_->size() > 1 && mode_->value() >= 0 && output_->value() >= 0 &&
                static_cast<size_t>(output_->value()) < outputs_.size() &&
                outputs_[output_->value()].current >= 0;
    if (page == 3)
        valid = valid && !layouts_.empty();
    if (active && valid)
        apply_->activate();
    else
        apply_->deactivate();
    if (!layout_->value())
        variant_->deactivate();
    if (access("/usr/bin/network-controller", X_OK))
        network_->deactivate();
    if (access("/usr/bin/openrc-controller", X_OK))
        services_->deactivate();
}
void App::reload() {
    Text error, language;
    Store store(directory_.c_str());
    locale_ok_ = store.read("locale.conf", locale_snapshot_, error) &&
                 read_language(locale_snapshot_.bytes.c_str(), language, error);
    if (locale_ok_) {
        language_->clear();
        language_->add(T("System default"));
        for (const auto &option : language_options)
            menu_item(language_, option.name);
        int index = 0;
        for (int i = 0; i < language_count; ++i)
            if (language == language_options[i].locale)
                index = i + 1;
        if (!index && !language.empty()) {
            menu_item(language_, language.c_str());
            index = language_count + 1;
        }
        language_->value(index);
    }
    Desktop desktop;
    config_ok_ = store.read("desktop.conf", desktop_snapshot_, error) &&
                 parse_desktop(desktop_snapshot_.bytes.c_str(), desktop, error);
    if (config_ok_) {
        layout_->value(0);
        for (size_t i = 0; i < layouts_.size(); ++i)
            if (layouts_[i].id == desktop.layout)
                layout_->value(i + 1);
        fill_variants();
        int index = layout_->value() - 1;
        if (index >= 0)
            for (size_t i = 0; i < layouts_[index].variants.size(); ++i)
                if (layouts_[index].variants[i].id == desktop.variant)
                    variant_->value(i + 1);
    }
    Text zone = current_timezone();
    timezone_->value(zone.empty() ? T("Custom localtime file or UTC default") : zone.c_str());
    for (int i = 1; i <= zone_list_->size(); ++i)
        if (!strcmp(zone_list_->text(i), zone.c_str()))
            zone_list_->value(i);
    if (!config_ok_ || !locale_ok_)
        status(T(error.c_str()));
    controls();
}
void App::fill_variants() {
    variant_->clear();
    variant_->add(T("Default"));
    int index = layout_->value() - 1;
    if (index >= 0 && static_cast<size_t>(index) < layouts_.size()) {
        for (size_t i = 0; i < layouts_[index].variants.size(); ++i)
            menu_item(variant_, layouts_[index].variants[i].label.c_str());
    }
    variant_->value(0);
    if (layout_->value())
        variant_->activate();
    else
        variant_->deactivate();
}
void App::filter_zones() {
    zone_list_->clear();
    const char *needle = zone_search_->value();
    Text current = current_timezone();
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (*needle && !strcasestr(zones_[i].c_str(), needle))
            continue;
        zone_list_->add(zones_[i].c_str());
        if (zones_[i] == current)
            zone_list_->value(zone_list_->size());
    }
}
void App::refresh_display() {
    if (busy())
        return;
    const char *argv[] = {"/usr/bin/xrandr", "--query", nullptr};
    if (!process_.start(argv)) {
        status(T("Cannot start xrandr."));
        return;
    }
    process_kind_ = QueryDisplay;
    operation_started_ = seconds();
    controls();
}
void App::fill_modes() {
    mode_->clear();
    current_mode_->value("");
    int index = output_->value();
    if (index >= 0 && static_cast<size_t>(index) < outputs_.size()) {
        const auto &output = outputs_[index];
        for (size_t i = 0; i < output.modes.size(); ++i) {
            char label[200];
            snprintf(label, sizeof(label), "%s   %s Hz", output.modes[i].name.c_str(),
                     output.modes[i].rate.c_str());
            mode_->add(label);
            if (static_cast<int>(i) == output.current)
                current_mode_->value(label);
        }
        mode_->value(output.current);
        if (output.current < 0)
            current_mode_->value(T("Output is disabled"));
    }
    controls();
}
void App::apply_callback(Fl_Widget *, void *p) {
    static_cast<App *>(p)->apply();
}
void App::apply() {
    if (busy())
        return;
    Text error, contents;
    int page = navigation_->value() - 1;
    bool started = false;
    if (page == 0) {
        int selected = language_->value();
        Text original;
        if (selected == language_count + 1)
            read_language(locale_snapshot_.bytes.c_str(), original, error);
        const char *chosen = selected == language_count + 1
                                 ? original.c_str()
                                 : (selected > 0 && selected <= language_count
                                        ? language_options[selected - 1].locale : "");
        if (selected < 0 || selected > language_count + 1 ||
            !update_language(locale_snapshot_.bytes.c_str(), chosen, contents, error)) {
            status(T(error.c_str()));
            return;
        }
        SaveRequest request{directory_.c_str(), "locale.conf", &locale_snapshot_, &contents};
        started = job_.start(save_worker, &request, display_fd());
        job_kind_ = Language;
    } else if (page == 1) {
        int selected = zone_list_->value();
        if (!selected) {
            status(T("Choose a time zone."));
            return;
        }
        const char *zone = zone_list_->text(selected);
        if (geteuid() == 0) {
            const char *argv[] = {Program, "--set-timezone", zone, nullptr};
            started = process_.start(argv);
        } else {
            const char *language = setlocale(LC_MESSAGES, nullptr);
            const char *argv[] = {Program, "--authenticate-timezone", zone,
                                  language ? language : "C", nullptr};
            started = process_.start(argv);
        }
        process_kind_ = Timezone;
        operation_started_ = seconds();
    } else if (page == 2 || page == 3) {
        Desktop desktop;
        if (!parse_desktop(desktop_snapshot_.bytes.c_str(), desktop, error)) {
            status(T(error.c_str()));
            return;
        }
        if (page == 3) {
            int index = layout_->value() - 1;
            desktop.layout.clear();
            desktop.variant.clear();
            if (index >= 0 && static_cast<size_t>(index) < layouts_.size()) {
                desktop.layout = layouts_[index].id;
                int v = variant_->value() - 1;
                if (v >= 0 && static_cast<size_t>(v) < layouts_[index].variants.size())
                    desktop.variant = layouts_[index].variants[v].id;
            }
            if (!encode_desktop(desktop, contents, error)) {
                status(T(error.c_str()));
                return;
            }
            SaveRequest save{directory_.c_str(), "desktop.conf", &desktop_snapshot_, &contents};
            KeyboardRequest request{save, desktop.layout.c_str(), desktop.variant.c_str()};
            started = desktop.layout.empty() ? job_.start(save_worker, &save, display_fd())
                                             : job_.start(keyboard_worker, &request, display_fd());
            job_kind_ = Keyboard;
        } else {
            int o = output_->value(), m = mode_->value();
            if (o < 0 || static_cast<size_t>(o) >= outputs_.size() || m < 0 ||
                static_cast<size_t>(m) >= outputs_[o].modes.size() || outputs_[o].current < 0)
                return;
            const auto &output = outputs_[o];
            const auto &next = output.modes[m];
            DisplaySetting after{output.name, next.name, next.rate};
            bool found = false;
            for (size_t i = 0; i < desktop.displays.size(); ++i)
                if (desktop.displays[i].output == output.name) {
                    desktop.displays[i] = after;
                    found = true;
                }
            if (!found && !desktop.displays.push(after)) {
                status(T("Out of memory."));
                return;
            }
            if (!encode_desktop(desktop, contents, error)) {
                status(T(error.c_str()));
                return;
            }
            SaveRequest save{directory_.c_str(), "desktop.conf", &desktop_snapshot_, &contents};
            DisplayRequest request{save, &after};
            started = job_.start(display_worker, &request, display_fd());
            job_kind_ = Display;
        }
    }
    status(started ? T("Applying…") : T("Cannot start the settings worker."));
    controls();
}
void App::confirm_display() {
    if (!preview_) {
        preview_ = new Fl_Double_Window(440, 178, T("Keep these display settings?"));
        preview_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->decide(false); },
                           this);
        preview_text_ = new Fl_Box(18, 18, 404, 70);
        preview_text_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        restore_ = new Fl_Button(154, 122, 130, 34, T("Restore"));
        restore_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->decide(false); },
                           this);
        keep_ = new Fl_Button(298, 122, 124, 34, T("Keep"));
        keep_->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->decide(true); }, this);
        preview_->end();
        preview_->set_modal();
        reliefos_fltk::style(preview_);
    }
    countdown_ = seconds() + 15;
    last_second_ = -1;
    keep_->activate();
    restore_->activate();
    preview_->position((Fl::w() - preview_->w()) / 2, (Fl::h() - preview_->h()) / 2);
    preview_->show();
    restore_->take_focus();
}
void App::decide(bool keep) {
    job_.decide(keep);
    keep_->deactivate();
    restore_->deactivate();
    preview_text_->copy_label(keep ? T("Saving display settings…")
                                   : T("Restoring the previous display mode…"));
    countdown_ = 0;
}
void App::tick(void *p) {
    auto *app = static_cast<App *>(p);
    app->pump();
    Fl::repeat_timeout(.05, tick, p);
}
void App::pump() {
    if (process_.running()) {
        process_.step(65536);
        if (process_kind_ == QueryDisplay && seconds() - operation_started_ > 10)
            process_.cancel();
        if (!process_.running()) {
            bool ok = !process_.exit_code() && !process_.exec_error() && !process_.truncated();
            Text error;
            if (process_kind_ == QueryDisplay) {
                if (ok)
                    ok = parse_outputs(process_.output(), outputs_, error);
                output_->clear();
                if (ok) {
                    for (size_t i = 0; i < outputs_.size(); ++i)
                        output_->add(outputs_[i].name.c_str());
                    output_->value(0);
                    fill_modes();
                } else {
                    outputs_ = Array<Output>{};
                    mode_->clear();
                    current_mode_->value("");
                }
            } else
                reload();
            if (!ok && process_kind_ == Timezone && process_.exit_code() == 3) {
                status(T("Time zone updated. Storage synchronization failed."));
                show_error(process_.errors());
            } else if (!ok) {
                const char *message = !error.empty() ? error.c_str()
                                                     : (*process_.errors() ? process_.errors()
                                                                           : "The command failed.");
                status(T("The operation failed."));
                show_error(message);
            } else if (process_kind_ == Timezone)
                status(T("System time zone updated."));
            process_kind_ = Idle;
            controls();
        }
    }
    Reply message;
    while (job_.poll(message)) {
        if (message.status == 'R')
            confirm_display();
        else {
            if (preview_)
                preview_->hide();
            countdown_ = 0;
            reload();
            if (message.status == 'E') {
                status(T("Settings could not be applied."));
                show_error(message.error);
            } else if (message.status == 'W') {
                status(T("Settings applied. Storage synchronization failed."));
                show_error(message.error);
            } else if (message.status == 'B')
                status(T("Previous display mode restored."));
            else
                status(job_kind_ == Language ? T("Language saved. Sign out and sign in to use it.")
                                             : T("Settings applied."));
            if (job_kind_ == Display)
                refresh_display();
            job_kind_ = Nothing;
            controls();
        }
    }
    if (countdown_ > 0) {
        int left = static_cast<int>(countdown_ - seconds() + 1);
        if (left < 0)
            left = 0;
        if (left != last_second_) {
            char text[256];
            snprintf(text, sizeof(text),
                     T("The previous display mode will be restored in %d seconds."), left);
            preview_text_->copy_label(text);
            last_second_ = left;
        }
    }
    static time_t last_time = 0;
    time_t now = time(nullptr);
    if (now != last_time) {
        last_time = now;
        struct tm local{};
        Text zone = current_timezone(), previous;
        bool had_tz = getenv("TZ") != nullptr;
        bool copied = !had_tz || previous.set(getenv("TZ"));
        // Display the system zone even when this process inherited a TZ
        // override. Restore the environment before launching any application.
        if (copied && !zone.empty())
            setenv("TZ", zone.c_str(), 1);
        tzset();
        localtime_r(&now, &local);
        char text[128];
        strftime(text, sizeof(text), "%Y-%m-%d  %H:%M:%S  %Z", &local);
        if (copied) {
            if (had_tz)
                setenv("TZ", previous.c_str(), 1);
            else
                unsetenv("TZ");
        }
        clock_->copy_label(text);
    }
    for (auto &pid : launched_) {
        int result;
        if (pid > 0 && waitpid(pid, &result, WNOHANG) == pid) {
            pid = -1;
            if (!WIFEXITED(result) || WEXITSTATUS(result))
                status(T("The requested application exited with an error."));
        }
    }
}
void App::launch(unsigned index) {
    if (index > 1 || launched_[index] > 0)
        return;
    const char *path = index ? "/usr/bin/openrc-controller" : "/usr/bin/network-controller";
    pid_t pid = fork();
    if (!pid) {
        if (display_fd() >= 0)
            ::close(display_fd());
        setsid();
        execl(path, path, static_cast<char *>(nullptr));
        _exit(127);
    }
    launched_[index] = pid;
    status(pid > 0 ? T("Application started.") : T("Cannot start the application."));
}
void App::close() {
    if (process_.running() && process_kind_ == Timezone) {
        status(T("Finish or cancel administrator authentication before closing."));
        return;
    }
    job_.close();
    if (preview_)
        preview_->hide();
    if (error_window_)
        error_window_->hide();
    window_->hide();
}
} // namespace rs
