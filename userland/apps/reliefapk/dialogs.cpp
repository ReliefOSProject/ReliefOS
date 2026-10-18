#define _POSIX_C_SOURCE 200809L
#include "window.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Return_Button.H>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ra {
namespace {
class TextWindow : public Fl_Double_Window {
    Fl_Text_Buffer buffer_;
    Fl_Text_Display *display_;

  public:
    TextWindow(const char *title, const char *content) : Fl_Double_Window(790, 520, title) {
        display_ = new Fl_Text_Display(12, 12, 766, 452);
        display_->buffer(&buffer_);
        display_->textfont(FL_HELVETICA);
        display_->textsize(15);
        display_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        buffer_.text(content);
        auto *close = new Fl_Button(666, 478, 112, 30, T("Close"));
        close->callback([](Fl_Widget *w, void *) { w->window()->do_callback(); });
        end();
        resizable(display_);
        reliefos_fltk::style(this);
    }
    ~TextWindow() { display_->buffer(nullptr); }
};
bool add_words(Array<Text> &out, const char *s) {
    for (size_t at = 0; s && s[at];) {
        while (s[at] == ' ' || s[at] == '\t')
            ++at;
        if (!s[at])
            break;
        size_t end = at;
        while (s[end] && s[end] != ' ' && s[end] != '\t')
            ++end;
        Text word;
        if (!word.set(s + at, end - at) || !out.push(word))
            return false;
        at = end;
    }
    return true;
}
bool settings_path(char *path, size_t cap, bool create) {
    const char *home = getenv("HOME");
    const char *base = getenv("XDG_CONFIG_HOME");
    char directory[2048];
    if (base && base[0] == '/')
        snprintf(directory, sizeof(directory), "%s", base);
    else if (home && home[0] == '/')
        snprintf(directory, sizeof(directory), "%s/.config", home);
    else
        return false;
    if (create && mkdir(directory, 0700) < 0 && errno != EEXIST)
        return false;
    return snprintf(path, cap, "%s/reliefapk.conf", directory) < static_cast<int>(cap);
}
} // namespace
void show_text(const char *title, const char *content) {
    auto *window = new TextWindow(title, content);
    window->callback([](Fl_Widget *w, void *) {
        w->hide();
        Fl::delete_widget(w);
    });
    window->show();
}
void App::show_auth() {
    if (!auth_window) {
        auth_window = new Fl_Double_Window(500, 226, T("Administrator authentication"));
        auth_description = new Fl_Output(18, 18, 464, 52);
        auth_description->box(FL_FLAT_BOX);
        auth_description->color(FL_BACKGROUND_COLOR);
        auth_description->textsize(15);
        password = new Fl_Secret_Input(126, 96, 356, 32, T("Password:"));
        auto *cancel = new Fl_Button(242, 166, 112, 32, T("Cancel"));
        cancel->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->answer_auth(false); }, this);
        auto *authenticate = new Fl_Return_Button(366, 166, 116, 32, T("Authenticate"));
        authenticate->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->answer_auth(true); }, this);
        auth_window->end();
        auth_window->set_modal();
        auth_window->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->answer_auth(false); }, this);
        reliefos_fltk::style(auth_window);
    }
    char message[256];
    struct passwd *user = getpwuid(getuid());
    snprintf(message, sizeof(message),
             auth_mode == AuthMode::RootPassword ? T("Enter the root password to authorize this operation.")
                                                 : T("Enter the sudo password for %s."),
             user ? user->pw_name : "");
    auth_description->value(message);
    password->value("");
    auth_window->show();
    password->take_focus();
    op_status->value(T("Waiting for administrator authentication..."));
    auth_window->redraw();
    Fl::flush();
    printf("RELIEFAPK_AUTH requested mode=%d\n", static_cast<int>(auth_mode));
    fflush(stdout);
}
void App::answer_auth(bool accept) {
    if (!auth_window)
        return;
    if (accept) {
        if (!operation.provide_password(password->value())) {
            fl_alert("%s", T("Cannot send the password to the system authentication service."));
            operation.cancel_auth();
        }
    } else
        operation.cancel_auth();
    size_t n = strlen(password->value());
    if (n) {
        volatile char *secret = const_cast<char *>(password->value());
        while (n)
            *secret++ = 0, --n;
    }
    password->value("");
    auth_window->hide();
}
void App::tool(Action action) {
    if (operation.running() || (mode != OperationMode::Idle && mode != OperationMode::Done)) {
        fl_alert("%s", T("Finish or close the current operation first."));
        return;
    }
    if (action == Action::World) {
        constexpr size_t limit = 1024 * 1024;
        FILE *file = fopen("/etc/apk/world", "rb");
        char *contents = static_cast<char *>(malloc(limit + 1));
        Text world;
        bool ok = false;
        if (file && contents) {
            size_t n = fread(contents, 1, limit + 1, file);
            ok = n <= limit && !ferror(file) && !memchr(contents, 0, n) && world.set(contents, n);
        }
        if (file)
            fclose(file);
        free(contents);
        if (ok)
            show_text(T("World constraints"), world.c_str());
        else
            fl_alert("%s", T("Cannot read world constraints."));
        return;
    }
    if (action == Action::Keys) {
        Fl_Text_Buffer b;
        const char *directories[] = {"/etc/apk/keys"};
        for (const char *dir : directories) {
            text_field(b, T("Directory"), dir);
            DIR *d = opendir(dir);
            if (!d)
                continue;
            while (auto *e = readdir(d)) {
                if (e->d_name[0] == '.')
                    continue;
                b.append("  ");
                b.append(e->d_name);
                b.append("\n");
            }
            closedir(d);
            b.append("\n");
        }
        char *s = b.text();
        show_text(action_name(action), s);
        free(s);
        return;
    }
    Array<Text> targets;
    Settings options = settings;
    const auto *p = selected();
    Text input;
    if (action == Action::Fetch) {
        const char *folder = fl_dir_chooser(T("Download packages to"), getenv("HOME"));
        if (!folder)
            return;
        targets.push(text(folder));
        const char *names = fl_input(T("Package names or version constraints, separated by spaces:"),
                                     p ? p->name.c_str() : "");
        if (!names || !*names)
            return;
        if (!add_words(targets, names))
            return;
        options.fetch_dependencies = fl_choice("%s", T("Only selected"), T("Include dependencies"), nullptr,
                                               T("Download package dependencies as well?")) == 1;
    } else if (action == Action::Verify || action == Action::Manifest) {
        const char *file = fl_file_chooser(T("Select APK package"), "*.apk", nullptr);
        if (!file)
            return;
        targets.push(text(file));
    } else if (action == Action::Owner) {
        const char *file = fl_input(T("Absolute path of the file:"), "/usr/bin/");
        if (!file || !*file)
            return;
        targets.push(text(file));
    } else if (action == Action::Version) {
        const char *one = fl_input(T("First version:"), "1.0-r0");
        if (!one || !*one)
            return;
        targets.push(text(one));
        const char *two = fl_input(T("Second version:"), "1.1-r0");
        if (!two || !*two)
            return;
        targets.push(text(two));
    } else if (action == Action::Dot) {
        if (p)
            targets.push(p->name);
        else {
            const char *name = fl_input(T("Package name:"), "");
            if (!name || !*name)
                return;
            targets.push(text(name));
        }
    }
    Command c;
    Text error;
    if (!make_command(action, targets, options, false, c, error)) {
        fl_alert("%s", error.c_str());
        return;
    }
    if (action == Action::CacheClean || action == Action::CacheDownload) {
        if (fl_choice("%s", T("Cancel"), T("Continue"), nullptr,
                      action == Action::CacheClean ? T("Clean unused apk cache entries?")
                                                   : T("Download missing packages into the apk cache?")) != 1)
            return;
    }
    show_operation(action_name(action));
    op_buffer.text("");
    launch(c, OperationMode::Tool);
}
void App::load_settings() {
    char path[2200];
    if (!settings_path(path, sizeof(path), false))
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        unsigned n;
        if (sscanf(line, "timeout=%u", &n) == 1 && n >= 1 && n <= 600)
            settings.timeout = n;
        else if (sscanf(line, "cache_max_age=%u", &n) == 1 && n <= 10080)
            settings.cache_max_age = n;
        else if (!strcmp(line, "offline=1"))
            settings.offline = true;
        else if (!strcmp(line, "auth=root"))
            auth_mode = AuthMode::RootPassword;
        else if (!strncmp(line, "cache_dir=/", 11))
            settings.cache_dir.set(line + 10);
    }
    fclose(f);
}
void App::save_settings() {
    char path[2200];
    if (!settings_path(path, sizeof(path), true))
        return;
    char temp[2240];
    snprintf(temp, sizeof(temp), "%s.XXXXXX", path);
    int fd = mkstemp(temp);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(temp);
        return;
    }
    fprintf(f, "offline=%u\ntimeout=%u\ncache_max_age=%u\nauth=%s\ncache_dir=%s\n", settings.offline ? 1 : 0,
            settings.timeout, settings.cache_max_age, auth_mode == AuthMode::Sudo ? "sudo" : "root",
            settings.cache_dir.c_str());
    bool ok = fflush(f) == 0 && fsync(fd) == 0;
    if (fclose(f))
        ok = false;
    if (ok)
        ok = rename(temp, path) == 0;
    if (!ok)
        unlink(temp);
}
namespace {
Fl_Input *cache_input = nullptr;
}
void App::show_settings() {
    if (!settings_window) {
        settings_window = new Fl_Double_Window(620, 340, T("ReliefAPK preferences"));
        offline = new Fl_Check_Button(20, 20, 570, 30,
                                      T("Offline: use installed packages and cached indexes only"));
        timeout_input = new Fl_Input(234, 72, 110, 30, T("Network timeout (seconds):"));
        age_input = new Fl_Input(234, 116, 110, 30, T("Index cache age (minutes):"));
        auth_choice = new Fl_Choice(234, 160, 350, 30, T("Administrator authentication:"));
        auth_choice->add(T("sudo — current user's password"));
        auth_choice->add(T("su — root password"));
        cache_input = new Fl_Input(234, 204, 350, 30, T("Cache directory:"));
        cache_input->tooltip(T("Leave empty to use the system apk cache."));
        auto *cancel = new Fl_Button(354, 282, 110, 32, T("Cancel"));
        cancel->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->settings_window->hide(); }, this);
        auto *save = new Fl_Return_Button(476, 282, 108, 32, T("Save"));
        save->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->apply_settings(); }, this);
        settings_window->end();
        reliefos_fltk::style(settings_window);
    }
    offline->value(settings.offline);
    char value[32];
    snprintf(value, sizeof(value), "%u", settings.timeout);
    timeout_input->value(value);
    snprintf(value, sizeof(value), "%u", settings.cache_max_age);
    age_input->value(value);
    auth_choice->value(auth_mode == AuthMode::Sudo ? 0 : 1);
    cache_input->value(settings.cache_dir.c_str());
    settings_window->show();
}
void App::apply_settings() {
    char *end = nullptr;
    unsigned long timeout = strtoul(timeout_input->value(), &end, 10);
    if (!*timeout_input->value() || *end || timeout < 1 || timeout > 600) {
        fl_alert("%s", T("Network timeout must be between 1 and 600 seconds."));
        return;
    }
    unsigned long age = strtoul(age_input->value(), &end, 10);
    if (!*age_input->value() || *end || age > 10080) {
        fl_alert("%s", T("Index cache age must be between 0 and 10080 minutes."));
        return;
    }
    const char *cache = cache_input->value();
    if (*cache) {
        if (cache[0] != '/' || strlen(cache) > 2048) {
            fl_alert("%s", T("Choose an absolute cache directory, or leave it empty."));
            return;
        }
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(cache); *p; ++p)
            if (*p < 32 || *p == 127) {
                fl_alert("%s", T("Invalid cache directory."));
                return;
            }
    }
    settings.offline = offline->value() != 0;
    settings.timeout = static_cast<unsigned>(timeout);
    settings.cache_max_age = static_cast<unsigned>(age);
    settings.cache_dir.set(cache);
    auth_mode = auth_choice->value() ? AuthMode::RootPassword : AuthMode::Sudo;
    save_settings();
    settings_window->hide();
    request_catalog();
}
class RepositoryDialog : public Fl_Double_Window {
  public:
    App *app;
    Repositories document;
    Array<Text> files;
    Array<unsigned> indices;
    Fl_Choice *file;
    Fl_Hold_Browser *list;
    Fl_Input *tag, *url;
    Fl_Check_Button *enabled;
    bool dirty = false;
    RepositoryDialog(App *owner) : Fl_Double_Window(850, 520, T("APK repositories")), app(owner) {
        file = new Fl_Choice(114, 14, 718, 30, T("Config file:"));
        files.push(text("/etc/apk/repositories"));
        DIR *dir = opendir("/etc/apk/repositories.d");
        if (dir) {
            while (auto *entry = readdir(dir)) {
                char path[1024];
                snprintf(path, sizeof(path), "/etc/apk/repositories.d/%s", entry->d_name);
                if (standard_repository_path(path))
                    files.push(text(path));
            }
            closedir(dir);
        }
        for (size_t i = 0; i < files.size(); ++i)
            file->add(files[i].c_str());
        file->value(0);
        file->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                if (d->dirty && fl_choice("%s", T("Cancel"), T("Discard changes"), nullptr,
                                          T("Discard the unsaved repository changes?")) != 1) {
                    for (size_t i = 0; i < d->files.size(); ++i)
                        if (!strcmp(d->files[i].c_str(), d->document.path.c_str()))
                            d->file->value(static_cast<int>(i));
                    return;
                }
                d->load();
            },
            this);
        list = new Fl_Hold_Browser(16, 58, 816, 240);
        list->format_char(0);
        list->textsize(14);
        list->callback([](Fl_Widget *, void *p) { static_cast<RepositoryDialog *>(p)->select(); }, this);
        url = new Fl_Input(114, 314, 718, 30, T("Address:"));
        tag = new Fl_Input(114, 354, 270, 30, T("Tag:"));
        tag->tooltip(T("Optional repository tag, without @"));
        enabled = new Fl_Check_Button(412, 354, 210, 30, T("Enabled"));
        enabled->value(1);
        auto *add = new Fl_Button(16, 402, 100, 30, T("Add"));
        add->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                if (!d->document.add(d->tag->value(), d->url->value(), d->enabled->value()))
                    fl_alert("%s", T("Invalid repository address or tag."));
                else {
                    d->dirty = true;
                    d->refresh();
                    d->list->value(d->list->size());
                }
            },
            this);
        auto *edit = new Fl_Button(126, 402, 110, 30, T("Update entry"));
        edit->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                int row = d->list->value();
                if (row < 1 || static_cast<size_t>(row) > d->indices.size())
                    return;
                if (!d->document.replace(d->indices[static_cast<size_t>(row - 1)], d->tag->value(),
                                         d->url->value(), d->enabled->value()))
                    fl_alert("%s", T("Invalid repository address or tag."));
                else {
                    d->dirty = true;
                    d->refresh();
                    d->list->value(row);
                }
            },
            this);
        auto *remove = new Fl_Button(246, 402, 100, 30, T("Remove"));
        remove->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                int row = d->list->value();
                if (row < 1 || static_cast<size_t>(row) > d->indices.size())
                    return;
                d->document.lines.erase(d->indices[static_cast<size_t>(row - 1)]);
                d->dirty = true;
                d->refresh();
            },
            this);
        auto *up = new Fl_Button(356, 402, 76, 30, T("Up"));
        up->callback([](Fl_Widget *, void *p) { static_cast<RepositoryDialog *>(p)->move(-1); }, this);
        auto *down = new Fl_Button(442, 402, 76, 30, T("Down"));
        down->callback([](Fl_Widget *, void *p) { static_cast<RepositoryDialog *>(p)->move(1); }, this);
        auto *source = new Fl_Button(530, 402, 138, 30, T("View source"));
        source->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                Text result;
                if (d->document.serialize(result))
                    show_text(d->document.path.c_str(), result.c_str());
            },
            this);
        auto *reload = new Fl_Button(16, 466, 110, 32, T("Reload"));
        reload->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                if (!d->dirty || fl_choice("%s", T("Cancel"), T("Discard changes"), nullptr,
                                           T("Discard the unsaved repository changes?")) == 1)
                    d->load();
            },
            this);
        auto *save = new Fl_Return_Button(584, 466, 128, 32, T("Save changes"));
        save->callback(
            [](Fl_Widget *, void *p) {
                auto *d = static_cast<RepositoryDialog *>(p);
                if (d->dirty)
                    d->app->save_repositories(d->document);
            },
            this);
        auto *close = new Fl_Button(724, 466, 108, 32, T("Close"));
        close->callback([](Fl_Widget *, void *p) { static_cast<RepositoryDialog *>(p)->close(); }, this);
        callback([](Fl_Widget *, void *p) { static_cast<RepositoryDialog *>(p)->close(); }, this);
        end();
        resizable(list);
        size_range(850, 520);
        reliefos_fltk::style(this);
        load();
    }
    void load() {
        int selected = file->value();
        if (selected < 0 || static_cast<size_t>(selected) >= files.size())
            return;
        if (!document.load(files[static_cast<size_t>(selected)].c_str())) {
            fl_alert("%s", document.error.c_str());
            return;
        }
        dirty = false;
        refresh();
    }
    void refresh() {
        list->clear();
        indices.clear();
        for (size_t i = 0; i < document.lines.size(); ++i) {
            const auto &r = document.lines[i];
            if (!r.recognized)
                continue;
            char line[4400];
            snprintf(line, sizeof(line), "[%s] %s%s%s%s", r.enabled ? "+" : " ", r.tag.empty() ? "" : "@",
                     r.tag.c_str(), r.tag.empty() ? "" : "  ", r.url.c_str());
            list->add(line);
            indices.push(static_cast<unsigned>(i));
        }
        select();
    }
    void select() {
        int row = list->value();
        if (row < 1 || static_cast<size_t>(row) > indices.size())
            return;
        const auto &r = document.lines[indices[static_cast<size_t>(row - 1)]];
        tag->value(r.tag.c_str());
        url->value(r.url.c_str());
        enabled->value(r.enabled);
    }
    void move(int direction) {
        int row = list->value(), next = row + direction;
        if (row < 1 || next < 1 || static_cast<size_t>(row) > indices.size() ||
            static_cast<size_t>(next) > indices.size())
            return;
        unsigned a = indices[static_cast<size_t>(row - 1)], b = indices[static_cast<size_t>(next - 1)];
        RepositoryLine saved = static_cast<RepositoryLine &&>(document.lines[a]);
        document.lines[a] = static_cast<RepositoryLine &&>(document.lines[b]);
        document.lines[b] = static_cast<RepositoryLine &&>(saved);
        dirty = true;
        refresh();
        list->value(next);
        select();
    }
    void close() {
        if (!active())
            return;
        if (dirty && fl_choice("%s", T("Cancel"), T("Discard changes"), nullptr,
                               T("Discard the unsaved repository changes?")) != 1)
            return;
        hide();
    }
};
void App::show_repositories() {
    if (!repositories)
        repositories = new RepositoryDialog(this);
    repositories->show();
}
void destroy_repositories(RepositoryDialog *dialog) { delete dialog; }
void repository_finished(RepositoryDialog *dialog, bool success) {
    if (!dialog)
        return;
    dialog->activate();
    if (success)
        dialog->load();
}
void App::save_repositories(Repositories &document) {
    if (operation.running() || (mode != OperationMode::Idle && mode != OperationMode::Done)) {
        fl_alert("%s", T("Finish or close the current operation first."));
        return;
    }
    Text content;
    if (!document.serialize(content)) {
        fl_alert("%s", T("Repository configuration is too large."));
        return;
    }
    Command c;
    c.privileged = true;
    c.mutating = true;
    c.cancelable = false;
    c.args.push(text(WorkerPath));
    c.args.push(text("--save-repositories"));
    c.args.push(document.path);
    c.args.push(document.original);
    c.args.push(content);
    show_operation(T("Save repositories"));
    op_buffer.text(T("Saving repository configuration. The previous file is kept as .reliefapk.bak.\n"));
    // Configuration contents are not printed into the command log.
    const char *args[6];
    for (size_t i = 0; i < c.args.size(); ++i)
        args[i] = c.args[i].c_str();
    args[5] = nullptr;
    out_seen = err_seen = 0;
    mode = OperationMode::Save;
    op_pending = true;
    apply_button->deactivate();
    cancel_button->deactivate();
    clear_button->deactivate();
    op_status->value(T("Saving repositories..."));
    if (!operation.start(args, true, auth_mode, false)) {
        op_pending = false;
        mode = OperationMode::Done;
        op_status->value(T("Cannot start the requested operation."));
        cancel_button->activate();
        return;
    }
    repositories->deactivate();
}
} // namespace ra
