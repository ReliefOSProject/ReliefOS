#include "../clipboard/manager.h"
#include "../fltk_support.h"
#include "../fltk_pixels.h"
#include "tray.h"
#include "image.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t interrupted;
void interrupt(int) { interrupted = 1; }

class Application;
class HistoryWindow : public Fl_Double_Window {
    Application &app_;
    Fl_Input *search_;
    Fl_Hold_Browser *list_;
    Fl_Text_Display *preview_;
    ImagePreview *image_;
    Fl_Text_Buffer buffer_;
    Fl_Button *use_, *remove_, *clear_;
    Fl_Check_Button *pause_, *persist_;
    Fl_Output *status_;
    clip::Array<uint64_t> rows_;
    uint64_t revision_ = UINT64_MAX;
    void details();
public:
    explicit HistoryWindow(Application &);
    ~HistoryWindow() override { preview_->buffer(nullptr); }
    void refresh(bool force = false);
    size_t selected() const;
    void status(const char *message) { status_->value(message); }
    void resize(int, int, int, int) override;
};

class Application {
    pid_t writer_ = -1;
    uint64_t disk_revision_ = UINT64_MAX, writing_revision_ = 0;
    bool disk_persist_ = false, writing_persist_ = false;
    double retry_ = 0;
public:
    clip::Manager manager;
    clip::StatePaths paths;
    HistoryWindow *window = nullptr;
    Tray tray{manager.selection};
    bool persist = true, quit = false;
    void save();
    int collect(bool block);
    void shutdown();
    void tick();
    void show() { window->refresh(true); window->show(); window->take_focus(); }
    void clear() {
        if (fl_choice("%s", T("Cancel"), T("Clear"), nullptr,
            T("Clear clipboard history and the current clipboard?")) != 1) return;
        manager.clear();
        window->refresh(true);
        window->status(T("Saving history..."));
    }
};

HistoryWindow::HistoryWindow(Application &app) : Fl_Double_Window(780, 550, T("Clipboard")), app_(app)
{
    size_range(660, 420);
    callback([](Fl_Widget *w, void *) { w->hide(); });
    search_ = new Fl_Input(76, 12, 692, 30, T("Search"));
    search_->when(FL_WHEN_CHANGED);
    search_->callback([](Fl_Widget *, void *p) { static_cast<HistoryWindow *>(p)->refresh(true); }, this);
    list_ = new Fl_Hold_Browser(12, 54, 330, 368);
    list_->format_char(0);
    list_->textsize(15);
    list_->callback([](Fl_Widget *, void *p) { static_cast<HistoryWindow *>(p)->details(); }, this);
    list_->when(FL_WHEN_CHANGED);
    preview_ = new Fl_Text_Display(354, 54, 414, 368);
    preview_->buffer(buffer_);
    preview_->textsize(15);
    preview_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    image_ = new ImagePreview(354, 54, 414, 368);
    image_->hide();
    use_ = new Fl_Button(12, 434, 150, 30, T("Copy to clipboard"));
    use_->shortcut(FL_Enter);
    use_->callback([](Fl_Widget *, void *p) {
        auto *self = static_cast<HistoryWindow *>(p);
        if (self->app_.manager.use(self->selected())) {
            self->status(T("Copied. Paste into another application."));
            self->hide();
        }
    }, this);
    remove_ = new Fl_Button(174, 434, 120, 30, T("Remove"));
    remove_->callback([](Fl_Widget *, void *p) {
        auto *self = static_cast<HistoryWindow *>(p);
        self->app_.manager.history.remove(self->selected());
        self->refresh(true);
    }, this);
    clear_ = new Fl_Button(306, 434, 120, 30, T("Clear history"));
    clear_->callback([](Fl_Widget *, void *p) { static_cast<HistoryWindow *>(p)->app_.clear(); }, this);
    pause_ = new Fl_Check_Button(12, 476, 220, 28, T("Pause recording"));
    pause_->callback([](Fl_Widget *w, void *p) {
        auto *self = static_cast<HistoryWindow *>(p);
        self->app_.manager.paused = static_cast<Fl_Check_Button *>(w)->value();
        self->status(self->app_.manager.paused ? T("Recording paused") : T("Recording resumed"));
    }, this);
    persist_ = new Fl_Check_Button(244, 476, 450, 28, T("Keep history after logout"));
    persist_->value(app.persist);
    persist_->callback([](Fl_Widget *w, void *p) {
        auto *self = static_cast<HistoryWindow *>(p);
        self->app_.persist = static_cast<Fl_Check_Button *>(w)->value();
        self->status(T("Saving history..."));
    }, this);
    status_ = new Fl_Output(12, 514, 756, 24);
    end();
    reliefos_fltk::style(this);
    refresh(true);
}

void HistoryWindow::resize(int x, int y, int w, int h)
{
    Fl_Double_Window::resize(x, y, w, h);
    int left = (w - 36) * 43 / 100;
    search_->resize(76, 12, w - 88, 30);
    list_->resize(12, 54, left, h - 182);
    preview_->resize(left + 24, 54, w - left - 36, h - 182);
    image_->resize(left + 24, 54, w - left - 36, h - 182);
    use_->position(12, h - 116);
    remove_->position(174, h - 116);
    clear_->position(306, h - 116);
    pause_->position(12, h - 74);
    persist_->resize(244, h - 74, w - 256, 28);
    status_->resize(12, h - 36, w - 24, 24);
}

size_t HistoryWindow::selected() const
{
    int row = list_->value();
    if (row <= 0 || size_t(row) > rows_.size()) return SIZE_MAX;
    for (size_t i = 0; i < app_.manager.history.entries.size(); ++i)
        if (app_.manager.history.entries[i].id == rows_[row - 1]) return i;
    return SIZE_MAX;
}

void HistoryWindow::details()
{
    size_t index = selected();
    if (index >= app_.manager.history.entries.size()) {
        use_->deactivate(); remove_->deactivate(); buffer_.text(""); image_->hide(); preview_->show(); return;
    }
    const clip::Entry &entry = app_.manager.history.entries[index];
    clip::Text text = clip::preview(entry);
    buffer_.text(text.c_str());
    if (image_->load(entry)) { preview_->hide(); image_->show(); }
    else { image_->hide(); preview_->show(); }
    use_->activate(); remove_->activate();
    char message[160];
    snprintf(message, sizeof(message), "%s  %zu / %zu   |   %.1f KiB", T("History"),
        index + 1, app_.manager.history.entries.size(), double(entry.size()) / 1024.);
    status(message);
}

void HistoryWindow::refresh(bool force)
{
    if (Fl::pushed() != pause_) pause_->value(app_.manager.paused);
    if (Fl::pushed() != persist_) persist_->value(app_.persist);
    if (!force && revision_ == app_.manager.history.revision) return;
    size_t index = selected();
    uint64_t selected_id = index < app_.manager.history.entries.size() ? app_.manager.history.entries[index].id : 0;
    rows_.clear();
    list_->clear();
    for (const auto &entry : app_.manager.history.entries) {
        if (!clip::matches(entry, search_->value()) || !rows_.push(entry.id)) continue;
        clip::Text text = clip::preview(entry);
        char label[320];
        size_t length = text.size() < sizeof(label) - 1 ? text.size() : sizeof(label) - 1;
        while (length && length < text.size() && (static_cast<unsigned char>(text.c_str()[length]) & 0xc0) == 0x80) --length;
        for (size_t i = 0; i < length; ++i) label[i] = text.c_str()[i] == '\n' || text.c_str()[i] == '\t' ? ' ' : text.c_str()[i];
        label[length] = 0;
        list_->add(*label ? label : T("Empty text"));
        if (entry.id == selected_id) list_->value(int(rows_.size()));
    }
    if (!list_->value() && rows_.size()) list_->value(1);
    if (rows_.empty()) list_->add(T("No clipboard history"));
    revision_ = app_.manager.history.revision;
    details();
}

void Application::save()
{
    uint64_t revision = persist ? manager.history.revision : 0;
    if (writer_ > 0 || clip::now() < retry_ || (revision == disk_revision_ && persist == disk_persist_)) return;
    writing_revision_ = revision;
    writing_persist_ = persist;
    writer_ = fork();
    if (writer_ == 0) {
        close(manager.selection.fd());
        if (fl_display) close(ConnectionNumber(fl_display));
        int result = clip::save_preferences(paths.config.c_str(), persist);
        if (!result) result = persist ? clip::save_history(paths.state.c_str(), manager.history) :
            clip::delete_history(paths.state.c_str());
        if (result) fprintf(stderr, "[reliefclipboard] state write failed (%d)\n", -result);
        _exit(result ? 1 : 0);
    }
    if (writer_ < 0) { retry_ = clip::now() + 5; window->status(T("Could not save clipboard history")); }
}

void Application::tick()
{
    manager.pump();
    collect(false);
    save();
    window->refresh();
}

int Application::collect(bool block)
{
    if (writer_ > 0) {
        int status = 0;
        pid_t done;
        do { done = waitpid(writer_, &status, block ? 0 : WNOHANG); } while (done < 0 && errno == EINTR);
        if (done == writer_) {
            writer_ = -1;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                disk_revision_ = writing_revision_;
                disk_persist_ = writing_persist_;
                window->status(T("History saved"));
            } else { retry_ = clip::now() + 5; window->status(T("Could not save clipboard history")); return -1; }
            return 1;
        }
    }
    return 0;
}

void Application::shutdown()
{
    manager.paused = true;
    manager.selection.cancel();
    collect(true);
    retry_ = 0;
    save();
    collect(true);
}

int forward(bool background)
{
    Display *display = XOpenDisplay(nullptr);
    if (!display) return -1;
    Window owner = XGetSelectionOwner(display, XInternAtom(display, "CLIPBOARD_MANAGER", False));
    if (!owner) { XCloseDisplay(display); return 0; }
    if (!background) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = owner;
        event.xclient.message_type = XInternAtom(display, "_RELIEFOS_CLIPBOARD_SHOW", False);
        event.xclient.format = 32;
        XSendEvent(display, owner, False, NoEventMask, &event);
        XSync(display, False);
    }
    XCloseDisplay(display);
    return 1;
}
}

int main(int argc, char **argv)
{
    bool daemon = argc == 2 && !strcmp(argv[1], "--daemon");
    if (argc > 2 || (argc == 2 && !daemon && strcmp(argv[1], "--show"))) {
        fputs("usage: reliefclipboard [--daemon | --show]\n", stderr);
        return 2;
    }
    int existing = forward(daemon);
    if (existing > 0) return 0;
    if (existing < 0) { fputs("[reliefclipboard] cannot open display\n", stderr); return 1; }
    reliefos_fltk::init("reliefclipboard");
    Application app;
    if (!app.paths.initialize() || !app.manager.initialize()) {
        fprintf(stderr, "[reliefclipboard] %s\n", app.manager.error());
        return 1;
    }
    int preferences = clip::load_preferences(app.paths.config.c_str(), app.persist);
    int history = app.persist ? clip::load_history(app.paths.state.c_str(), app.manager.history) : 0;
    app.window = new HistoryWindow(app);
    if ((preferences && preferences != -ENOENT) || (history && history != -ENOENT))
        app.window->status(T("Could not load clipboard history"));
    app.manager.show = [](void *p) { static_cast<Application *>(p)->show(); };
    app.manager.events = [](const XEvent &event, void *p) { static_cast<Application *>(p)->tray.event(event); };
    app.manager.context = &app;
    app.tray.context = &app;
    app.tray.clicked = [](bool context_menu, int x, int y, void *p) {
        auto *app = static_cast<Application *>(p);
        if (!context_menu) { app->show(); return; }
        Fl_Menu_Item menu[5]{};
        menu[0].text = T("Show clipboard");
        menu[1].text = app->manager.paused ? T("Resume recording") : T("Pause recording");
        menu[2].text = T("Clear history");
        menu[3].text = T("Quit");
        int row_height = 0, screen_x, screen_y, screen_width, screen_height;
        menu[0].measure(&row_height, nullptr);
        Fl::screen_work_area(screen_x, screen_y, screen_width, screen_height, x, y);
        if (y > screen_y + screen_height / 2) y -= 4 * (row_height + 6) + 8;
        const Fl_Menu_Item *item = menu->popup(x, y);
        if (!item) return;
        int action = int(item - menu);
        if (action == 0) app->show();
        else if (action == 1) app->manager.paused = !app->manager.paused;
        else if (action == 2) app->clear();
        else if (action == 3) app->quit = true;
    };
    app.tray.initialize();
    Fl::add_fd(app.manager.selection.fd(), [](int, void *p) { static_cast<Application *>(p)->tick(); }, &app);
    Fl::add_clipboard_notify([](int source, void *p) {
        if (source == 1) static_cast<Application *>(p)->manager.changed();
    }, &app);
    signal(SIGTERM, interrupt); signal(SIGINT, interrupt); signal(SIGHUP, interrupt);
    if (!app.manager.selection.owner() && !app.manager.history.entries.empty()) app.manager.use(0);
    if (!daemon) app.show();
    while (!app.quit && !interrupted && !app.manager.stopped) {
        Fl::wait(.1);
        app.tick();
    }
    app.window->hide();
    app.shutdown();
    Fl::remove_fd(app.manager.selection.fd());
    delete app.window;
    return 0;
}
