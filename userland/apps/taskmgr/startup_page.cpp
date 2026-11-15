#include "startup_page.h"
#include "autostart.h"
#include "fltk_support.h"
#include <gio/gio.h>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace autostart;
namespace {
class StartupBrowser : public Fl_Hold_Browser {
    StartupPage *page_;
public:
    StartupBrowser(int x, int y, int w, int h, StartupPage *page)
        : Fl_Hold_Browser(x, y, w, h), page_(page) {}
    int handle(int event) override {
        if (event == FL_KEYDOWN) {
            if (Fl::event_key() == FL_F + 5) { page_->refresh(true); return 1; }
            if (Fl::event_key() == FL_Delete) { page_->remove(); return 1; }
            if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) { page_->edit(); return 1; }
        }
        int result = Fl_Hold_Browser::handle(event);
        if (event == FL_RELEASE && Fl::event_clicks() && Fl::event_button() == FL_LEFT_MOUSE) page_->edit();
        return result;
    }
};

const char *state_name(State state)
{
    switch (state) {
    case State::Ready: return T("Enabled");
    case State::Disabled: return T("Disabled");
    case State::OtherDesktop: return T("Other desktop");
    case State::MissingProgram: return T("Program unavailable");
    case State::Invalid: return T("Invalid entry");
    }
    return "";
}

Text display_text(const char *input)
{
    char *valid = g_utf8_make_valid(input, -1);
    for (char *p = valid; *p; ++p) if (static_cast<unsigned char>(*p) < 32 || *p == 127) *p = ' ';
    Text result = ra::text(valid);
    g_free(valid);
    return result;
}

void close_inherited(int kept)
{
    DIR *descriptors = opendir("/proc/self/fd");
    if (descriptors) {
        struct dirent *item;
        while ((item = readdir(descriptors))) {
            char *end = nullptr;
            long fd = strtol(item->d_name, &end, 10);
            if (end && !*end && fd >= 3 && fd != kept && fd != dirfd(descriptors)) close(fd);
        }
        closedir(descriptors);
        return;
    }
    struct rlimit limit;
    rlim_t end = getrlimit(RLIMIT_NOFILE, &limit) ? 1024 : limit.rlim_cur;
    if (end == RLIM_INFINITY || end > 65536) end = 65536;
    for (int n = 3; static_cast<rlim_t>(n) < end; ++n) if (n != kept) close(n);
}

struct Editor : Fl_Double_Window {
    Fl_Input *name, *command, *comment, *directory, *try_exec;
    Fl_Check_Button *terminal, *enabled;
    Fl_Return_Button *save;
    bool accepted = false;
    bool original_dbus = false;
    Text original_command;
    Editor(const Entry *entry) : Fl_Double_Window(700, 352, entry ? T("Edit startup application") : T("Add startup application")) {
        name = new Fl_Input(136, 16, 548, 28, T("Name"));
        command = new Fl_Input(136, 56, 456, 28, T("Command"));
        auto *browse = new Fl_Button(600, 56, 84, 28, T("Browse…"));
        browse->callback([](Fl_Widget *, void *data) {
            auto *editor = static_cast<Editor *>(data);
            Fl_Native_File_Chooser chooser;
            chooser.title(T("Choose executable"));
            chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
            if (!chooser.show() && chooser.filename()) {
                Text command = executable_command(chooser.filename());
                editor->command->value(command.c_str());
            }
        }, this);
        command->tooltip(T("Desktop Entry Exec syntax. Quote paths with spaces using double quotes."));
        comment = new Fl_Input(136, 96, 548, 28, T("Description"));
        directory = new Fl_Input(136, 136, 456, 28, T("Working folder"));
        auto *folder = new Fl_Button(600, 136, 84, 28, T("Browse…"));
        folder->callback([](Fl_Widget *, void *data) {
            auto *editor = static_cast<Editor *>(data);
            Fl_Native_File_Chooser chooser;
            chooser.title(T("Choose working folder"));
            chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
            if (!chooser.show() && chooser.filename()) editor->directory->value(chooser.filename());
        }, this);
        try_exec = new Fl_Input(136, 176, 548, 28, T("Required program"));
        try_exec->tooltip(T("Optional TryExec. Start only when this executable is available."));
        enabled = new Fl_Check_Button(136, 216, 250, 28, T("Start at login"));
        terminal = new Fl_Check_Button(400, 216, 284, 28, T("Run in terminal"));
        auto *note = new Fl_Box(136, 252, 548, 40, T("Changes apply at the next login."));
        note->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        auto *cancel = new Fl_Button(496, 308, 88, 28, T("Cancel"));
        cancel->callback([](Fl_Widget *, void *data) { static_cast<Editor *>(data)->hide(); }, this);
        save = new Fl_Return_Button(596, 308, 88, 28, T("Save"));
        save->callback([](Fl_Widget *, void *data) {
            auto *editor = static_cast<Editor *>(data);
            Text error;
            if (!editor->name->value()[0]) { fl_alert("%s", T("Enter a valid UTF-8 name.")); return; }
            bool dbus_only = editor->original_dbus && editor->original_command.empty() && !editor->command->value()[0];
            if (!dbus_only && !valid_command(editor->command->value(), error)) { fl_alert("%s", T(error.c_str())); return; }
            editor->accepted = true;
            editor->hide();
        }, this);
        end();
        set_modal();
        reliefos_fltk::style(this);
        if (entry) {
            original_dbus = entry->dbus; original_command = entry->command;
            name->value(entry->name.c_str()); command->value(entry->command.c_str());
            comment->value(entry->comment.c_str()); directory->value(entry->directory.c_str());
            try_exec->value(entry->try_exec.c_str()); terminal->value(entry->terminal);
        }
        enabled->value(!entry || !entry->hidden);
        callback([](Fl_Widget *widget, void *) { widget->hide(); });
    }
    Edit values() const {
        Edit edit;
        edit.name.set(name->value()); edit.command.set(command->value());
        edit.comment.set(comment->value()); edit.directory.set(directory->value());
        edit.try_exec.set(try_exec->value()); edit.terminal = terminal->value(); edit.enabled = enabled->value();
        return edit;
    }
};
} // namespace

struct StartupPage::Impl {
    void (*status)(const char *);
    Fl_Choice *account;
    Fl_Box *user_label, *headers[4], *empty;
    Fl_Button *add, *edit, *toggle, *remove, *refresh;
    StartupBrowser *list;
    Fl_Multiline_Output *details;
    Array<uid_t> users;
    Snapshot snapshot;
    Text selection;
    int widths[5] = {};
    int fd = -1;
    pid_t child = -1;
    GByteArray *received = nullptr;
    bool modal = false;
    gint64 last_refresh = 0;

    Entry *selected() {
        int at = list->value() - 1;
        return at >= 0 && static_cast<size_t>(at) < snapshot.entries.size() ? &snapshot.entries[at] : nullptr;
    }
    void controls() {
        Entry *entry = selected();
        bool idle = fd < 0 && !modal;
        Fl_Widget *widgets[] = {account, add, refresh};
        for (auto *widget : widgets) { if (idle) widget->activate(); else widget->deactivate(); }
        if (idle && entry) { edit->activate(); toggle->activate(); } else { edit->deactivate(); toggle->deactivate(); }
        if (idle && entry && entry->personal) remove->activate(); else remove->deactivate();
        toggle->copy_label(entry && entry->hidden ? T("Enable") : T("Disable"));
        remove->copy_label(entry && entry->fallback ? T("Restore default") : T("Remove"));
        if (!entry) { details->value(""); return; }
        char *text = g_strdup_printf("%s\n%s  %s%s%s%s%s%s%s", entry->source.c_str(), state_name(entry->state),
            entry->comment.c_str(), entry->only_show.empty() ? "" : "  OnlyShowIn=", entry->only_show.c_str(),
            entry->not_show.empty() ? "" : "  NotShowIn=", entry->not_show.c_str(),
            entry->try_exec.empty() ? "" : "  TryExec=", entry->try_exec.c_str());
        char *valid = g_utf8_make_valid(text, -1);
        details->value(valid);
        g_free(valid);
        g_free(text);
    }
    void render() {
        int top = list->topline();
        list->clear();
        for (const auto &entry : snapshot.entries) {
            const char *command_label = entry.command.c_str();
            if (entry.command.empty()) command_label = entry.dbus ? T("D-Bus activation") : T("Not specified");
            Text name = display_text(entry.name.c_str()), command = display_text(command_label);
            char *row = g_strdup_printf("%s\t%s\t%s\t%s", name.c_str(), state_name(entry.state),
                entry.personal ? (entry.fallback ? T("Personal override") : T("Personal")) : T("System"), command.c_str());
            list->add(row);
            g_free(row);
        }
        // Re-select by filename, independently of sorting or entries inserted above it.
        list->value(0);
        for (size_t n = 0; n < snapshot.entries.size(); ++n)
            if (snapshot.entries[n].id == selection) list->value(static_cast<int>(n + 1));
        if (top > 0 && top <= list->size()) list->topline(top);
        if (snapshot.entries.empty()) {
            empty->copy_label(snapshot.error.empty() ? T("No startup applications. Use Add to start a program at login.") : T(snapshot.error.c_str()));
            empty->show();
        } else empty->hide();
        controls();
    }
    static void read_ready(int, void *data) { static_cast<Impl *>(data)->receive(); }
    void receive() {
        char buffer[32768];
        bool done = false, failed = false;
        for (;;) {
            ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count <= 0) { done = true; failed = count < 0; break; }
            if (received->len + static_cast<size_t>(count) > MaxSnapshotBytes) { done = true; failed = true; break; }
            g_byte_array_append(received, reinterpret_cast<guint8 *>(buffer), count);
        }
        if (!done) return;
        Fl::remove_fd(fd);
        close(fd); fd = -1;
        if (failed) kill(child, SIGTERM);
        int result = 0;
        while (waitpid(child, &result, 0) < 0 && errno == EINTR) {}
        child = -1;
        Text input;
        input.set(reinterpret_cast<const char *>(received->data), received->len);
        g_byte_array_unref(received); received = nullptr;
        Snapshot next;
        if (failed || !WIFEXITED(result) || WEXITSTATUS(result) || !decode(input, next)) {
            status(T("Could not read startup settings."));
            controls();
            return;
        }
        if (!next.selected.empty()) selection = next.selected;
        snapshot = static_cast<Snapshot &&>(next);
        render();
        status(snapshot.error.empty() ? T("Startup settings refreshed") : T(snapshot.error.c_str()));
    }
    void job(Request &request) {
        if (fd >= 0 || modal || account->value() < 0) return;
        request.uid = users[account->value()];
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) { status(T("Could not start the settings worker.")); return; }
        child = fork();
        if (child < 0) { close(sockets[0]); close(sockets[1]); status(T("Could not start the settings worker.")); return; }
        if (!child) {
            close(sockets[0]);
            // This child never talks to X11. Close inherited display and application descriptors.
            close_inherited(sockets[1]);
            Snapshot next;
            Paths paths;
            if (switch_user(request.uid, next.error) && paths.initialize(next.error)) {
                Text change_error;
                if (request.action != Action::Scan) change(paths, request, next.selected, change_error);
                scan(paths, next);
                if (!change_error.empty()) next.error = change_error;
            }
            Text output;
            if (!encode(next, output)) _exit(1);
            size_t used = 0;
            while (used < output.size()) {
                ssize_t count = write(sockets[1], output.c_str() + used, output.size() - used);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) _exit(1);
                used += count;
            }
            close(sockets[1]);
            _exit(0);
        }
        close(sockets[1]);
        fd = sockets[0];
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        received = g_byte_array_new();
        Fl::add_fd(fd, FL_READ, read_ready, this);
        last_refresh = g_get_monotonic_time();
        controls();
        status(T("Reading startup settings…"));
    }
};

StartupPage::StartupPage(int x, int y, int w, int h, void (*status)(const char *))
    : Fl_Group(x, y, w, h, T("Startup applications")), impl_(new Impl{})
{
    Impl &p = *impl_;
    p.status = status;
    begin();
    p.user_label = new Fl_Box(0, 0, 0, 0, T("User"));
    p.user_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    p.account = new Fl_Choice(0, 0, 0, 0);
    auto append_user = [&](uid_t uid, const char *name) {
        if (!p.users.push(uid)) return;
        char label[256];
        snprintf(label, sizeof(label), "%s (UID %u)", name, static_cast<unsigned>(uid));
        p.account->add(label, 0, nullptr, nullptr, 0);
        if (uid == getuid()) p.account->value(p.users.size() - 1);
    };
    struct passwd *current = getpwuid(getuid());
    append_user(getuid(), current ? current->pw_name : "?");
    if (!geteuid()) {
        setpwent();
        struct passwd *user;
        while ((user = getpwent())) {
            if (user->pw_uid != getuid() && user->pw_uid >= 1000 && user->pw_uid != 65534 &&
                user->pw_dir[0] == '/' && p.users.size() < 256) append_user(user->pw_uid, user->pw_name);
        }
        endpwent();
    }
    p.account->callback([](Fl_Widget *, void *data) {
        auto *page = static_cast<StartupPage *>(data);
        page->impl_->selection.clear();
        page->impl_->snapshot.entries.clear();
        page->impl_->render();
        page->refresh(true);
    }, this);
    p.add = new Fl_Button(0, 0, 0, 0, T("Add…"));
    p.add->callback([](Fl_Widget *, void *data) { static_cast<StartupPage *>(data)->edit(true); }, this);
    p.edit = new Fl_Button(0, 0, 0, 0, T("Edit…"));
    p.edit->callback([](Fl_Widget *, void *data) { static_cast<StartupPage *>(data)->edit(); }, this);
    p.toggle = new Fl_Button(0, 0, 0, 0, T("Disable"));
    p.toggle->callback([](Fl_Widget *, void *data) { static_cast<StartupPage *>(data)->toggle(); }, this);
    p.remove = new Fl_Button(0, 0, 0, 0, T("Remove"));
    p.remove->callback([](Fl_Widget *, void *data) { static_cast<StartupPage *>(data)->remove(); }, this);
    p.refresh = new Fl_Button(0, 0, 0, 0, T("Refresh"));
    p.refresh->callback([](Fl_Widget *, void *data) { static_cast<StartupPage *>(data)->refresh(true); }, this);
    const char *labels[] = {T("Name"), T("Status"), T("Source"), T("Command")};
    for (size_t n = 0; n < 4; ++n) {
        p.headers[n] = new Fl_Box(0, 0, 0, 0, labels[n]);
        p.headers[n]->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    p.list = new StartupBrowser(0, 0, 0, 0, this);
    p.list->format_char(0); p.list->column_char('\t'); p.list->column_widths(p.widths);
    p.list->when(FL_WHEN_CHANGED);
    p.list->tooltip(T("Double-click or press Enter to edit. Delete removes personal entries or restores a system default."));
    p.list->callback([](Fl_Widget *, void *data) {
        auto *p = static_cast<Impl *>(data);
        Entry *entry = p->selected();
        p->selection = entry ? entry->id : Text{};
        p->controls();
    }, &p);
    p.empty = new Fl_Box(0, 0, 0, 0, T("Reading startup settings…"));
    p.empty->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    p.details = new Fl_Multiline_Output(0, 0, 0, 0);
    p.details->wrap(1);
    p.details->tooltip(T("System entries are changed through personal overrides. Changes apply at the next login."));
    end();
    resizable(nullptr);
    resize(x, y, w, h);
    p.controls();
}

StartupPage::~StartupPage()
{
    if (impl_->fd >= 0) {
        Fl::remove_fd(impl_->fd);
        close(impl_->fd);
        kill(impl_->child, SIGTERM);
        while (waitpid(impl_->child, nullptr, 0) < 0 && errno == EINTR) {}
    }
    if (impl_->received) g_byte_array_unref(impl_->received);
    delete impl_;
}

void StartupPage::refresh(bool force)
{
    if (!force && g_get_monotonic_time() - impl_->last_refresh < 2000000) return;
    Request request;
    impl_->job(request);
}

void StartupPage::edit(bool create)
{
    Impl &p = *impl_;
    if (p.fd >= 0 || p.modal || (!create && !p.selected())) return;
    Request request;
    request.action = create ? Action::Create : Action::Edit;
    if (!create) request.original = *p.selected();
    p.modal = true;
    p.controls();
    Editor editor(create ? nullptr : &request.original);
    editor.show();
    editor.name->take_focus();
    while (editor.shown()) Fl::wait();
    p.modal = false;
    p.controls();
    if (editor.accepted) { request.edit = editor.values(); p.job(request); }
}

void StartupPage::toggle()
{
    Impl &p = *impl_;
    if (p.fd >= 0 || p.modal || !p.selected()) return;
    Request request;
    request.action = Action::Toggle;
    request.original = *p.selected();
    request.edit.enabled = request.original.hidden;
    p.job(request);
}

void StartupPage::remove()
{
    Impl &p = *impl_;
    if (p.fd >= 0 || p.modal || !p.selected() || !p.selected()->personal) return;
    Request request;
    request.action = Action::Remove;
    request.original = *p.selected();
    p.modal = true;
    p.controls();
    int confirmed = fl_choice("%s", T("Cancel"), request.original.fallback ? T("Restore default") : T("Remove"), nullptr,
        request.original.fallback ? T("Remove the personal override and restore the system startup entry? This may enable it again.") : T("Remove the selected personal startup application?"));
    p.modal = false;
    p.controls();
    if (confirmed == 1) p.job(request);
}

void StartupPage::resize(int x, int y, int w, int h)
{
    Fl_Group::resize(x, y, w, h);
    if (!impl_ || !impl_->details) return;
    Impl &p = *impl_;
    int left = x + 8, width = w - 16;
    p.user_label->resize(left, y + 10, 68, 28);
    p.account->resize(left + 72, y + 10, width - 72, 28);
    Fl_Button *buttons[] = {p.add, p.edit, p.toggle, p.remove};
    int button_widths[] = {88, 88, 100, 144};
    int at = left;
    for (size_t n = 0; n < 4; ++n) { buttons[n]->resize(at, y + 48, button_widths[n], 30); at += button_widths[n] + 8; }
    p.refresh->resize(left + width - 88, y + 48, 88, 30);
    p.widths[0] = width * 25 / 100; p.widths[1] = 154; p.widths[2] = 150;
    p.widths[3] = width - p.widths[0] - p.widths[1] - p.widths[2] - 28;
    p.widths[4] = 0;
    at = left + 5;
    for (size_t n = 0; n < 4; ++n) { p.headers[n]->resize(at, y + 88, p.widths[n], 22); at += p.widths[n]; }
    p.list->resize(left, y + 112, width, h - 190);
    p.empty->resize(left + 16, y + 140, width - 32, 56);
    p.details->resize(left, y + h - 68, width, 58);
    p.list->redraw();
}
