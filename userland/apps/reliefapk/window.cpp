#include "window.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_draw.H>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
namespace ra {
namespace {
void tick_cb(void *p) {
    static_cast<App *>(p)->tick();
    Fl::repeat_timeout(.03, tick_cb, p);
}
void search_cb(void *p) { static_cast<App *>(p)->filter_rows(); }
void detail_cb(void *p) { static_cast<App *>(p)->request_details(); }
void menu_action(Fl_Widget *widget, void *data) {
    auto *app = static_cast<App *>(widget->user_data());
    if (!app)
        return;
    int value = static_cast<int>(reinterpret_cast<intptr_t>(data));
    if (value < 100) {
        Action a = static_cast<Action>(value);
        if (a == Action::Update || a == Action::CacheClean || a == Action::CacheDownload ||
            static_cast<int>(a) >= static_cast<int>(Action::Fetch))
            app->tool(a);
        else
            app->enqueue(a);
        return;
    }
    switch (value) {
    case 100:
        app->open_local();
        break;
    case 101:
        app->show_queue();
        break;
    case 102:
        app->show_repositories();
        break;
    case 103:
        app->show_settings();
        break;
    case 104:
        app->close_app();
        break;
    case 105:
        show_text(T("ReliefAPK help"),
                  T("Search by package name or description. Select one or more packages, choose an "
                    "operation, then review the pending changes.\n\nPreview runs apk --simulate. Apply "
                    "executes the changes in the displayed order. Each operation is checked again before "
                    "execution.\n\nRefresh indexes after changing repositories or when available packages "
                    "are missing. Installed packages remain available offline.\n\nAdministrator operations "
                    "use your system sudo or root authentication policy. Passwords are never "
                    "saved.\n\nCtrl+F: search    F5: refresh list    Ctrl+Enter: preview changes"));
        break;
    case 107: {
        const char *name = fl_input(T("Package name, version constraint or repository tag:"), "");
        if (name && *name) {
            app->queue_target(Action::Install, name);
            app->show_queue();
        }
        break;
    }
    case 106:
        show_text(T("About ReliefAPK"),
                  T("ReliefAPK\nGraphical front end for Alpine Package Keeper\n\nC++ / FLTK — "
                    "ReliefOS\nPackage queries and transactions are performed by the system apk command."));
        break;
    }
}
void add_menu(App *app, const char *label, int shortcut, int action) {
    app->menu->add(label, shortcut, menu_action, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
}
bool matches_text(const char *s, const char *term) {
    size_t n = strlen(term);
    if (!n)
        return true;
    for (; *s; ++s)
        if (!strncasecmp(s, term, n))
            return true;
    return false;
}
const Array<Package> *sort_packages = nullptr;
const App *sort_app = nullptr;
int sort_column = 0;
bool sort_descending = false;
int compare_rows(const void *a, const void *b) {
    const Package &x = (*sort_packages)[*static_cast<const unsigned *>(a)],
                  &y = (*sort_packages)[*static_cast<const unsigned *>(b)];
    bool installed_column = sort_column == 1 || sort_column == 3;
    const Package *installed_x = installed_column ? sort_app->installed_package(x.name.c_str()) : nullptr,
                  *installed_y = installed_column ? sort_app->installed_package(y.name.c_str()) : nullptr;
    int c;
    if (sort_column == 4)
        c = x.installed_size < y.installed_size ? -1 : x.installed_size > y.installed_size ? 1 : 0;
    else if (sort_column == 1)
        c = strcasecmp(installed_x ? installed_x->version.c_str() : x.installed ? x.version.c_str() : "",
                       installed_y ? installed_y->version.c_str() : y.installed ? y.version.c_str() : "");
    else if (sort_column == 2)
        c = strcasecmp(x.version.c_str(), y.version.c_str());
    else if (sort_column == 3)
        c = static_cast<int>(installed_x || x.installed) - static_cast<int>(installed_y || y.installed);
    else if (sort_column == 5)
        c = strcasecmp(x.description.c_str(), y.description.c_str());
    else
        c = strcasecmp(x.name.c_str(), y.name.c_str());
    if (!c)
        c = strcmp(x.name.c_str(), y.name.c_str());
    return sort_descending ? -c : c;
}
void sanitize(char *out, size_t cap, const char *in) {
    size_t n = 0;
    for (; *in && n + 1 < cap; ++in) {
        unsigned char c = static_cast<unsigned char>(*in);
        out[n++] = c < 32 || c == 127 ? ' ' : *in;
    }
    out[n] = 0;
}
void print_command(Fl_Text_Buffer &buffer, const Command &command) {
    buffer.append("$ ");
    for (size_t i = 0; i < command.args.size(); ++i) {
        const char *s = command.args[i].c_str();
        if (i)
            buffer.append(" ");
        if (strchr(s, ' ') || strchr(s, '\n'))
            buffer.append("'");
        buffer.append(s);
        if (strchr(s, ' ') || strchr(s, '\n'))
            buffer.append("'");
    }
    buffer.append("\n");
}
} // namespace
const char *action_name(Action action) {
    switch (action) {
    case Action::Install:
        return T("Install");
    case Action::Remove:
        return T("Remove");
    case Action::Upgrade:
        return T("Upgrade selected");
    case Action::Reinstall:
        return T("Reinstall");
    case Action::Fix:
        return T("Repair");
    case Action::UpgradeAll:
        return T("Upgrade system");
    case Action::Update:
        return T("Refresh indexes");
    case Action::CacheClean:
        return T("Clean cache");
    case Action::CacheDownload:
        return T("Download cache");
    case Action::Fetch:
        return T("Download packages");
    case Action::Verify:
        return T("Verify APK file");
    case Action::Audit:
        return T("Audit system files");
    case Action::Stats:
        return T("Package statistics");
    case Action::Policy:
        return T("Repository policy");
    case Action::Contents:
        return T("Files");
    case Action::Depends:
        return T("Dependencies");
    case Action::ReverseDepends:
        return T("Reverse dependencies");
    case Action::Owner:
        return T("Find file owner");
    case Action::World:
        return T("World constraints");
    case Action::Version:
        return T("Compare versions");
    case Action::Dot:
        return T("Dependency graph (DOT)");
    case Action::Manifest:
        return T("Package checksums");
    case Action::Keys:
        return T("Trusted keys");
    }
    return "";
}
void text_field(Fl_Text_Buffer &b, const char *label, const char *value) {
    b.append(label);
    b.append(": ");
    b.append(value && *value ? value : T("None"));
    b.append("\n\n");
}
PackageTable::PackageTable(int x, int y, int w, int h, App *owner) : Fl_Table_Row(x, y, w, h), app(owner) {
    type(SELECT_MULTI);
    cols(6);
    rows(0);
    row_header(0);
    col_header(1);
    col_header_height(30);
    row_height_all(29);
    col_resize(1);
    row_resize(0);
    col_width_all(112);
    col_width(0, 155);
    col_width(1, 98);
    col_width(2, 98);
    col_width(3, 72);
    col_width(4, 84);
    col_width(5, 320);
    when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    callback([](Fl_Widget *w, void *) {
        auto *t = static_cast<PackageTable *>(w);
        if (t->callback_context() == CONTEXT_COL_HEADER) {
            int col = t->callback_col();
            if (col < 0)
                return;
            sort_descending = sort_column == col ? !sort_descending : false;
            sort_column = col;
            t->app->filter_rows();
        } else
            t->app->selection_changed();
    });
    end();
}
void PackageTable::draw_cell(TableContext context, int row, int col, int x, int y, int w, int h) {
    if (context == CONTEXT_STARTPAGE) {
        fl_font(FL_HELVETICA, 14);
        return;
    }
    if (context == CONTEXT_COL_HEADER) {
        const char *labels[] = {T("Package"), T("Installed version"), T("Available version"), T("Status"),
                                T("Size"),    T("Description")};
        fl_draw_box(FL_THIN_UP_BOX, x, y, w, h, FL_BACKGROUND_COLOR);
        fl_color(FL_FOREGROUND_COLOR);
        fl_push_clip(x + 5, y, w - 10, h);
        fl_draw(labels[col], x + 7, y, w - 14, h, FL_ALIGN_LEFT);
        fl_pop_clip();
        return;
    }
    if (context != CONTEXT_CELL || row < 0 || static_cast<size_t>(row) >= app->visible.size())
        return;
    const auto &p = app->packages[app->visible[static_cast<size_t>(row)]];
    const auto *installed = app->installed_package(p.name.c_str());
    bool is_installed = installed || p.installed;
    fl_push_clip(x, y, w, h);
    bool chosen = row_selected(row) != 0;
    Fl_Color background =
        chosen ? FL_SELECTION_COLOR : (row % 2 ? fl_lighter(FL_BACKGROUND2_COLOR) : FL_BACKGROUND2_COLOR);
    fl_color(background);
    fl_rectf(x, y, w, h);
    fl_color(fl_contrast(FL_FOREGROUND_COLOR, background));
    Text size = format_size(p.installed_size);
    const char *value = "";
    switch (col) {
    case 0:
        value = p.name.c_str();
        break;
    case 1:
        value = installed ? installed->version.c_str() : p.installed ? p.version.c_str() : "—";
        break;
    case 2:
        value = p.version.c_str();
        break;
    case 3:
        value = is_installed ? T("Installed") : T("Available");
        break;
    case 4:
        value = size.c_str();
        break;
    case 5:
        value = p.description.c_str();
        break;
    }
    char plain[2048];
    sanitize(plain, sizeof(plain), value);
    fl_draw(plain, x + 7, y, w - 14, h, FL_ALIGN_LEFT);
    fl_color(fl_color_average(FL_BACKGROUND_COLOR, FL_BACKGROUND2_COLOR, .7));
    fl_line(x, y + h - 1, x + w, y + h - 1);
    fl_pop_clip();
}
App::App(const char *path) : Fl_Double_Window(1000, 690, "ReliefAPK") {
    if (path)
        initial_path.set(path);
    load_settings();
    user_data(this);
    menu = new Fl_Menu_Bar(0, 0, w(), 27);
    menu->user_data(this);
    add_menu(this, T("&Package/Install"), 0, static_cast<int>(Action::Install));
    add_menu(this, T("&Package/Remove"), 0, static_cast<int>(Action::Remove));
    add_menu(this, T("&Package/Upgrade selected"), 0, static_cast<int>(Action::Upgrade));
    add_menu(this, T("&Package/Reinstall"), 0, static_cast<int>(Action::Reinstall));
    add_menu(this, T("&Package/Repair"), 0, static_cast<int>(Action::Fix));
    add_menu(this, T("&Package/Install by name or version..."), 0, 107);
    add_menu(this, T("&Package/Install local APK..."), 0, 100);
    add_menu(this, T("&Package/Pending changes..."), 0, 101);
    add_menu(this, T("&Package/Quit"), FL_CTRL + 'q', 104);
    add_menu(this, T("&Tools/Repositories..."), 0, 102);
    add_menu(this, T("&Tools/Preferences..."), 0, 103);
    const char *labels[] = {
        T("&Tools/Download packages..."), T("&Tools/Verify APK file..."),     T("&Tools/Find file owner..."),
        T("&Tools/World constraints"),    T("&Tools/Package statistics"),     T("&Tools/Audit system files"),
        T("&Tools/Compare versions..."),  T("&Tools/Dependency graph (DOT)"), T("&Tools/Package checksums"),
        T("&Tools/Trusted keys"),         T("&Tools/Clean cache..."),         T("&Tools/Download cache...")};
    Action actions[] = {Action::Fetch,    Action::Verify, Action::Owner,      Action::World,
                        Action::Stats,    Action::Audit,  Action::Version,    Action::Dot,
                        Action::Manifest, Action::Keys,   Action::CacheClean, Action::CacheDownload};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        add_menu(this, labels[i], 0, static_cast<int>(actions[i]));
    add_menu(this, T("&Help/How to use ReliefAPK"), FL_F + 1, 105);
    add_menu(this, T("&Help/About ReliefAPK"), 0, 106);
    refresh_button = new Fl_Button(10, 35, 94, 32, T("Refresh"));
    refresh_button->tooltip(T("Refresh the displayed package list (F5)"));
    refresh_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->request_catalog(); }, this);
    index_button = new Fl_Button(112, 35, 138, 32, T("Refresh indexes"));
    index_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->tool(Action::Update); }, this);
    upgrade_button = new Fl_Button(258, 35, 130, 32, T("Upgrade system"));
    upgrade_button->callback(
        [](Fl_Widget *, void *p) { static_cast<App *>(p)->queue_target(Action::UpgradeAll, ""); }, this);
    local_button = new Fl_Button(396, 35, 138, 32, T("Install local APK"));
    local_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->open_local(); }, this);
    queue_button = new Fl_Button(766, 35, 224, 32, T("Pending changes (0)"));
    queue_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->show_queue(); }, this);
    search = new Fl_Input(169, 80, 821, 30, T("Search:"));
    search->align(FL_ALIGN_LEFT);
    search->tooltip(T("Filter package names and descriptions (Ctrl+F)"));
    search->when(FL_WHEN_CHANGED);
    search->callback(
        [](Fl_Widget *, void *p) {
            Fl::remove_timeout(search_cb, p);
            Fl::add_timeout(.3, search_cb, p);
        },
        this);
    tile = new Fl_Tile(10, 122, 980, 510);
    sidebar = new Fl_Hold_Browser(10, 122, 150, 510);
    sidebar->format_char(0);
    sidebar->textsize(15);
    const char *filters[] = {T("All packages"), T("Installed"), T("Upgradable"), T("World packages and dependencies"),
                             T("Orphaned")};
    for (const char *s : filters)
        sidebar->add(s);
    sidebar->value(2);
    sidebar->callback(
        [](Fl_Widget *w, void *p) {
            auto *a = static_cast<App *>(p);
            a->wanted_filter = static_cast<Fl_Hold_Browser *>(w)->value() - 1;
            a->request_catalog();
        },
        this);
    table = new PackageTable(168, 122, 538, 510, this);
    detail_group = new Fl_Group(714, 122, 276, 510);
    tabs = new Fl_Tabs(714, 122, 276, 510);
    const char *tabnames[] = {T("Details"), T("Dependencies"), T("Files"), T("Policy")};
    for (int i = 0; i < 4; ++i) {
        auto *group = new Fl_Group(714, 150, 276, 482, tabnames[i]);
        auto *view = new Fl_Text_Display(720, 158, 264, 468);
        view->textfont(FL_HELVETICA);
        view->textsize(14);
        view->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        view->buffer(&detail_buffers[i]);
        detail_views[i] = view;
        group->resizable(view);
        group->end();
    }
    tabs->callback(
        [](Fl_Widget *w, void *p) {
            auto *a = static_cast<App *>(p);
            auto *t = static_cast<Fl_Tabs *>(w);
            for (int i = 0; i < 4; ++i)
                if (t->value() == a->detail_views[i]->parent())
                    a->detail_kind = i;
            a->selection_changed();
        },
        this);
    tabs->end();
    detail_group->resizable(tabs);
    detail_group->end();
    tile->end();
    install_button = new Fl_Button(168, 640, 102, 30, T("Install"));
    install_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->enqueue(Action::Install); },
                             this);
    remove_button = new Fl_Button(278, 640, 102, 30, T("Remove"));
    remove_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->enqueue(Action::Remove); },
                            this);
    selected_upgrade = new Fl_Button(388, 640, 132, 30, T("Upgrade selected"));
    selected_upgrade->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->enqueue(Action::Upgrade); },
                               this);
    repair_button = new Fl_Button(528, 640, 102, 30, T("Repair"));
    repair_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->enqueue(Action::Fix); }, this);
    status = new Fl_Output(10, 674, 980, 20);
    status->box(FL_FLAT_BOX);
    status->textsize(13);
    status->color(FL_BACKGROUND_COLOR);
    end();
    resizable(tile);
    size_range(900, 620);
    callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->close_app(); }, this);
    reliefos_fltk::style(this);
    status->textsize(13);
    set_status(T("Loading installed packages..."));
    layout();
    selection_changed();
    Fl::add_timeout(.03, tick_cb, this);
}
App::~App() {
    Fl::remove_timeout(tick_cb, this);
    Fl::remove_timeout(search_cb, this);
    Fl::remove_timeout(detail_cb, this);
    for (auto *view : detail_views)
        if (view)
            view->buffer(nullptr);
    if (op_view)
        op_view->buffer(nullptr);
    delete op_window;
    delete auth_window;
    delete queue_window;
    delete settings_window;
    destroy_repositories(repositories);
}
void App::layout() {
    menu->resize(0, 0, w(), 27);
    queue_button->resize(w() - 234, 35, 224, 32);
    search->resize(169, 80, w() - 179, 30);
    tile->resize(10, 122, w() - 20, h() - 192);
    int y = h() - 58;
    install_button->position(168, y);
    remove_button->position(278, y);
    selected_upgrade->position(388, y);
    repair_button->position(528, y);
    status->resize(10, h() - 20, w() - 20, 20);
}
void App::resize(int x, int y, int width, int height) {
    Fl_Double_Window::resize(x, y, width, height);
    layout();
}
int App::handle(int e) {
    if (e == FL_SHORTCUT) {
        int key = Fl::event_key();
        if ((Fl::event_state() & FL_CTRL) && key == 'f') {
            search->take_focus();
            return 1;
        }
        if (key == FL_F + 5) {
            request_catalog();
            return 1;
        }
        if ((Fl::event_state() & FL_CTRL) && key == FL_Enter) {
            preview_queue();
            return 1;
        }
    }
    return Fl_Double_Window::handle(e);
}
void App::set_status(const char *s) { status->value(s ? s : ""); }
const Package *App::installed_package(const char *name) const {
    size_t lo = 0, hi = installed.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(installed[mid].name.c_str(), name);
        if (!c)
            return &installed[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}
const Package *App::selected() const {
    for (size_t r = 0; r < visible.size(); ++r)
        if (table->row_selected(static_cast<int>(r)))
            return &packages[visible[r]];
    return nullptr;
}
void App::request_catalog() {
    catalog_second = wanted_filter == 1 ? -1 : wanted_filter;
    if (query.running()) {
        query.cancel();
        query_pending = true;
        return;
    }
    query_pending = false;
    begin_catalog(1);
}
void App::begin_catalog(int filter) {
    query_parsing = false;
    query_filter = filter;
    Command c;
    Text error;
    if (!query_command(query_filter, settings, c, error)) {
        set_status(error.c_str());
        return;
    }
    Array<const char *> args;
    if (!args.resize(c.args.size() + 1)) {
        set_status(T("Out of memory"));
        return;
    }
    for (size_t i = 0; i < c.args.size(); ++i)
        args[i] = c.args[i].c_str();
    args[c.args.size()] = nullptr;
    if (!query.start(args.data())) {
        set_status(T("Cannot start apk. Check that /sbin/apk is installed."));
        return;
    }
    set_status(T("Reading packages..."));
}
void App::filter_rows() {
    visible.clear();
    const char *term = search->value();
    for (size_t i = 0; i < packages.size(); ++i) {
        const auto &p = packages[i];
        if (matches_text(p.name.c_str(), term) || matches_text(p.description.c_str(), term))
            if (!visible.push(static_cast<unsigned>(i))) {
                set_status(T("Out of memory"));
                break;
            }
    }
    sort_packages = &packages;
    sort_app = this;
    if (visible.size() > 1)
        qsort(visible.data(), visible.size(), sizeof(unsigned), compare_rows);
    table->select_all_rows(0);
    table->rows(static_cast<int>(visible.size()));
    table->redraw();
    selection_changed();
    char s[200];
    snprintf(s, sizeof(s), T("%zu packages shown · %zu total%s"), visible.size(), packages.size(),
             settings.offline ? T(" · Offline") : "");
    set_status(s);
}
void App::selection_changed() {
    detail_parsing = false;
    selected_count = 0;
    for (size_t i = 0; i < visible.size(); ++i)
        if (table->row_selected(static_cast<int>(i)))
            ++selected_count;
    bool busy = mode != OperationMode::Idle && mode != OperationMode::Done;
    Fl_Button *buttons[] = {install_button, remove_button, selected_upgrade, repair_button};
    for (auto *b : buttons)
        if (selected_count && !busy)
            b->activate();
        else
            b->deactivate();
    Fl::remove_timeout(detail_cb, this);
    if (selected_count)
        Fl::add_timeout(.15, detail_cb, this);
    else
        for (auto &buffer : detail_buffers)
            buffer.text(T("Select a package to view its details."));
}
void App::request_details() {
    const auto *p = selected();
    if (!p)
        return;
    if (detail.running()) {
        detail.cancel();
        Fl::add_timeout(.1, detail_cb, this);
        return;
    }
    detail_parsing = false;
    detail_name = p->name;
    detail_version = p->version;
    detail_query_kind = detail_kind;
    Command c;
    Text error;
    if (detail_kind == 3) {
        Array<Text> targets;
        targets.push(p->name);
        if (!make_command(Action::Policy, targets, settings, false, c, error)) {
            detail_buffers[3].text(error.c_str());
            return;
        }
    } else if (detail_kind == 2) {
        c.args.push(text("/sbin/apk"));
        c.args.push(text("--no-network"));
        c.args.push(text("--legacy-info"));
        c.args.push(text("info"));
        c.args.push(text("--contents"));
        c.args.push(text("--"));
        c.args.push(p->name);
    } else {
        if (!metadata_command(p->name.c_str(), settings, c, error)) {
            detail_buffers[detail_kind].text(error.c_str());
            return;
        }
    }
    const char *args[80];
    for (size_t i = 0; i < c.args.size(); ++i)
        args[i] = c.args[i].c_str();
    args[c.args.size()] = nullptr;
    detail_buffers[detail_kind].text(T("Reading package details..."));
    if (!detail.start(args))
        detail_buffers[detail_kind].text(T("Cannot start apk."));
}
void App::tick() {
    if (query.running()) {
        query.step();
        if (!query.running()) {
            if (query_pending) {
                request_catalog();
            } else if (query.exit_code() != 0 || query.truncated()) {
                set_status(T("Package query failed. Refresh indexes or check the operation log."));
                if (!operation.running())
                    show_text(T("Package query"),
                              query.errors_size() ? query.errors() : T("Cannot read package information."));
            } else if (parser.begin(query.output(), query.output_size()))
                query_parsing = true;
            else
                set_status(parser.error().c_str());
        }
    } else if (query_pending)
        request_catalog();
    if (query_parsing) {
        if (!parser.step(96)) {
            query_parsing = false;
            set_status(parser.error().c_str());
        } else if (parser.done()) {
            query_parsing = false;
            Array<Package> parsed = parser.take();
            if (query_filter == 1) {
                installed.clear();
                installed.reserve(parsed.size());
                for (size_t i = 0; i < parsed.size(); ++i)
                    installed.push(parsed[i]);
            }
            if (query_filter == 1 && catalog_second >= 0) {
                int next = catalog_second;
                catalog_second = -1;
                begin_catalog(next);
                return;
            }
            packages = static_cast<Array<Package> &&>(parsed);
            filter_rows();
            if (query.errors_size()) {
                set_status(
                    T("Some repository indexes are unavailable. Refresh indexes to see all packages."));
            }
            Fl::flush();
            printf("RELIEFAPK_QUERY filter=%d rows=%zu\n", query_filter, packages.size());
            fflush(stdout);
        }
    }
    if (detail.running()) {
        detail.step();
        if (!detail.running()) {
            if (!selected() || strcmp(selected()->name.c_str(), detail_name.c_str())) {
            } else if (detail.exit_code() != 0 || detail.truncated())
                detail_buffers[detail_query_kind].text(
                    detail.errors_size() ? detail.errors() : T("No package details available."));
            else if (detail_query_kind == 3 || detail_query_kind == 2)
                detail_buffers[detail_query_kind].text(detail.output());
            else if (detail_parser.begin(detail.output(), detail.output_size()))
                detail_parsing = true;
            else
                detail_buffers[detail_query_kind].text(detail_parser.error().c_str());
        }
    }
    if (detail_parsing) {
        if (!detail_parser.step(8)) {
            detail_parsing = false;
            detail_buffers[detail_kind].text(detail_parser.error().c_str());
        } else if (detail_parser.done()) {
            detail_parsing = false;
            Array<Package> result = detail_parser.take();
            const Package *matching = nullptr;
            for (size_t i = 0; i < result.size(); ++i)
                if (!strcmp(result[i].name.c_str(), detail_name.c_str()) &&
                    !strcmp(result[i].version.c_str(), detail_version.c_str())) {
                    matching = &result[i];
                    break;
                }
            if (!matching) {
                detail_buffers[detail_kind].text(T("No package details available."));
            } else {
                const auto &p = *matching;
                detail_buffers[0].text("");
                text_field(detail_buffers[0], T("Package"), p.name.c_str());
                text_field(detail_buffers[0], T("Version"), p.version.c_str());
                text_field(detail_buffers[0], T("Description"), p.description.c_str());
                text_field(detail_buffers[0], T("Architecture"), p.arch.c_str());
                text_field(detail_buffers[0], T("License"), p.license.c_str());
                text_field(detail_buffers[0], T("Home page"), p.url.c_str());
                text_field(detail_buffers[0], T("Repository"), p.repositories.c_str());
                Text size = format_size(p.installed_size);
                text_field(detail_buffers[0], T("Installed size"), size.c_str());
                detail_buffers[1].text("");
                text_field(detail_buffers[1], T("Dependencies"), p.depends.c_str());
                text_field(detail_buffers[1], T("Reverse dependencies"), p.reverse_depends.c_str());
                text_field(detail_buffers[1], T("Provides"), p.provides.c_str());
            }
        }
    }
    if (operation.running()) {
        operation.step();
        if (operation.output_size() > out_seen) {
            append_log(operation.output() + out_seen, operation.output_size() - out_seen);
            out_seen = operation.output_size();
        }
        if (operation.errors_size() > err_seen) {
            append_log(operation.errors() + err_seen, operation.errors_size() - err_seen);
            err_seen = operation.errors_size();
        }
        if (operation.password_requested() && (!auth_window || !auth_window->shown()))
            show_auth();
        if (!operation.running())
            finish_operation();
    }
}
void App::queue_target(Action a, const char *target) {
    if (mode != OperationMode::Idle && mode != OperationMode::Done) {
        fl_alert("%s", T("Finish or close the current operation first."));
        return;
    }
    if (a != Action::UpgradeAll &&
        !(valid_constraint(target) || (a == Action::Install && valid_constraint(target, true)))) {
        fl_alert("%s", T("Invalid package name, constraint or APK path."));
        return;
    }
    for (size_t i = 0; i < queue.size(); ++i)
        if (!strcmp(queue[i].target.c_str(), target)) {
            queue[i].action = a;
            update_queue();
            return;
        }
    QueueEntry e;
    e.action = a;
    if (!e.target.set(target) || !queue.push(e)) {
        fl_alert("%s", T("Out of memory"));
        return;
    }
    update_queue();
}
void App::enqueue(Action action) {
    if (action == Action::UpgradeAll) {
        queue_target(action, "");
        return;
    }
    if (!selected_count) {
        fl_alert("%s", T("Select one or more packages first."));
        return;
    }
    for (size_t r = 0; r < visible.size(); ++r)
        if (table->row_selected(static_cast<int>(r))) {
            const auto &p = packages[visible[r]];
            bool is_installed = p.installed || installed_package(p.name.c_str());
            if (action != Action::Install && !is_installed)
                continue;
            queue_target(action, p.name.c_str());
        }
    update_queue();
}
void App::update_queue() {
    char s[100];
    snprintf(s, sizeof(s), T("Pending changes (%zu)"), queue.size());
    queue_button->copy_label(s);
    if (queue_list) {
        queue_list->clear();
        for (size_t i = 0; i < queue.size(); ++i) {
            char row[2048];
            snprintf(row, sizeof(row), "%s  %s", action_name(queue[i].action), queue[i].target.c_str());
            queue_list->add(row);
        }
    }
    char message[180];
    snprintf(message, sizeof(message), T("%zu pending changes. Review before applying."), queue.size());
    set_status(message);
}
void App::show_queue() {
    if (!queue_window) {
        queue_window = new Fl_Double_Window(680, 420, T("Pending changes"));
        queue_list = new Fl_Hold_Browser(12, 12, 656, 332);
        queue_list->format_char(0);
        queue_list->textsize(15);
        auto *remove = new Fl_Button(12, 360, 142, 32, T("Remove selected"));
        remove->callback(
            [](Fl_Widget *, void *p) {
                auto *a = static_cast<App *>(p);
                if (a->mode != OperationMode::Idle && a->mode != OperationMode::Done)
                    return;
                int row = a->queue_list->value();
                if (row > 0 && static_cast<size_t>(row) <= a->queue.size())
                    a->queue.erase(static_cast<size_t>(row - 1));
                a->update_queue();
            },
            this);
        auto *clear = new Fl_Button(164, 360, 100, 32, T("Clear all"));
        clear->callback(
            [](Fl_Widget *, void *p) {
                auto *a = static_cast<App *>(p);
                if (a->mode == OperationMode::Idle || a->mode == OperationMode::Done) {
                    a->queue.clear();
                    a->update_queue();
                }
            },
            this);
        auto *preview = new Fl_Return_Button(408, 360, 144, 32, T("Preview changes"));
        preview->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->preview_queue(); }, this);
        auto *close = new Fl_Button(564, 360, 104, 32, T("Close"));
        close->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->queue_window->hide(); }, this);
        queue_window->end();
        queue_window->resizable(queue_list);
        reliefos_fltk::style(queue_window);
    }
    update_queue();
    queue_window->show();
}
void App::preview_queue() {
    if (operation.running() || mode == OperationMode::Await || mode == OperationMode::Preview ||
        mode == OperationMode::Recheck || mode == OperationMode::Execute) {
        if (op_window)
            op_window->show();
        return;
    }
    if (queue.empty()) {
        fl_alert("%s", T("There are no pending changes."));
        return;
    }
    groups.clear();
    Action order[] = {Action::Install, Action::Upgrade, Action::Reinstall,
                      Action::Fix,     Action::Remove,  Action::UpgradeAll};
    for (auto action : order)
        for (size_t i = 0; i < queue.size(); ++i)
            if (queue[i].action == action) {
                groups.push(action);
                break;
            }
    if (queue_window)
        queue_window->hide();
    show_operation(T("Review package changes"));
    op_buffer.text(T("Operations run in the order shown. Separate apk transactions are checked again before "
                     "execution.\n\n"));
    current_group = 0;
    op_error = false;
    mode = OperationMode::Preview;
    start_group(true);
    selection_changed();
}
void App::start_group(bool simulate) {
    if (current_group >= groups.size())
        return;
    Array<Text> targets;
    for (size_t i = 0; i < queue.size(); ++i)
        if (queue[i].action == groups[current_group] && !queue[i].target.empty())
            targets.push(queue[i].target);
    Command c;
    Text error;
    if (!make_command(groups[current_group], targets, settings, simulate, c, error)) {
        append_log(error.c_str(), error.size());
        mode = OperationMode::Done;
        op_error = true;
        apply_button->deactivate();
        return;
    }
    op_buffer.append("\n");
    op_buffer.append(action_name(groups[current_group]));
    op_buffer.append(simulate ? T(" — simulation\n") : T(" — executing\n"));
    launch(c, mode);
}
bool App::launch(Command &c, OperationMode next) {
    if (operation.running()) {
        fl_alert("%s", T("Another operation is running."));
        return false;
    }
    const char *args[512];
    if (c.args.size() >= 512) {
        mode = OperationMode::Done;
        op_error = true;
        op_pending = false;
        op_status->value(T("Too many packages in one operation."));
        apply_button->deactivate();
        clear_button->activate();
        cancel_button->copy_label(T("Close"));
        cancel_button->activate();
        selection_changed();
        fl_alert("%s", T("Too many packages in one operation."));
        return false;
    }
    for (size_t i = 0; i < c.args.size(); ++i)
        args[i] = c.args[i].c_str();
    args[c.args.size()] = nullptr;
    print_command(op_buffer, c);
    out_seen = err_seen = 0;
    mode = next;
    op_pending = true;
    apply_button->deactivate();
    clear_button->deactivate();
    cancel_button->copy_label(c.cancelable ? T("Cancel") : T("Please wait"));
    if (c.cancelable)
        cancel_button->activate();
    else
        cancel_button->deactivate();
    progress->value(0);
    op_status->value(next == OperationMode::Preview || next == OperationMode::Recheck
                         ? T("Simulating changes...")
                         : T("Running apk..."));
    if (!operation.start(args, c.privileged, auth_mode, c.cancelable)) {
        op_buffer.append(T("\nCannot start the requested operation.\n"));
        mode = OperationMode::Done;
        op_pending = false;
        cancel_button->copy_label(T("Close"));
        cancel_button->activate();
        return false;
    }
    return true;
}
void App::show_operation(const char *title) {
    if (!op_window) {
        op_window = new Fl_Double_Window(850, 550, title);
        op_status = new Fl_Output(14, 14, 822, 30);
        op_status->box(FL_FLAT_BOX);
        op_status->color(FL_BACKGROUND_COLOR);
        progress = new Fl_Progress(14, 51, 822, 14);
        progress->minimum(0);
        progress->maximum(100);
        progress->selection_color(fl_rgb_color(75, 135, 80));
        op_view = new Fl_Text_Display(14, 78, 822, 400);
        op_view->textfont(FL_COURIER);
        op_view->textsize(14);
        op_view->buffer(&op_buffer);
        op_view->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        auto *save = new Fl_Button(14, 497, 132, 32, T("Save log..."));
        save->callback(
            [](Fl_Widget *, void *p) {
                auto *a = static_cast<App *>(p);
                const char *path = fl_file_chooser(T("Save operation log"), "*.txt", "reliefapk-log.txt");
                if (!path)
                    return;
                FILE *f = fopen(path, "wb");
                if (!f) {
                    fl_alert("%s", T("Cannot save the log."));
                    return;
                }
                char *s = a->op_buffer.text();
                size_t n = strlen(s);
                bool ok = fwrite(s, 1, n, f) == n;
                free(s);
                if (fclose(f))
                    ok = false;
                if (!ok)
                    fl_alert("%s", T("Cannot save the log."));
            },
            this);
        clear_button = new Fl_Button(158, 497, 130, 32, T("Clear log"));
        clear_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->op_buffer.text(""); }, this);
        apply_button = new Fl_Return_Button(548, 497, 158, 32, T("Apply changes"));
        apply_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->apply_changes(); }, this);
        cancel_button = new Fl_Button(718, 497, 118, 32, T("Close"));
        cancel_button->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->close_operation(); }, this);
        op_window->end();
        op_window->resizable(op_view);
        op_window->size_range(850, 550);
        op_window->callback([](Fl_Widget *, void *p) { static_cast<App *>(p)->close_operation(); }, this);
        reliefos_fltk::style(op_window);
    }
    op_window->copy_label(title);
    op_window->show();
    op_status->value("");
    apply_button->deactivate();
    clear_button->activate();
    cancel_button->copy_label(T("Close"));
    cancel_button->activate();
    progress->value(0);
}
void App::append_log(const char *bytes, size_t n) {
    if (!n)
        return;
    if (mode == OperationMode::Execute) {
        for (size_t i = 0; i < n; ++i)
            if (bytes[i] == '(') {
                unsigned done = 0, total = 0;
                if (sscanf(bytes + i, "(%u/%u)", &done, &total) == 2 && total && done <= total) {
                    double fraction = static_cast<double>(done) / total;
                    progress->value(groups.size() ? 100 * (current_group + fraction) / groups.size()
                                                  : 100 * fraction);
                }
            }
    }
    size_t start = n > 512 * 1024 ? n - 512 * 1024 : 0;
    char *copy = static_cast<char *>(malloc(n - start + 1));
    if (!copy)
        return;
    size_t used = 0;
    for (size_t i = start; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0 || c == 27)
            continue;
        if (c == '\r')
            c = '\n';
        copy[used++] = static_cast<char>(c);
    }
    copy[used] = 0;
    op_buffer.append(copy);
    free(copy);
    if (op_buffer.length() > 600 * 1024)
        op_buffer.remove(0, op_buffer.length() - 512 * 1024);
    op_view->insert_position(op_buffer.length());
    op_view->show_insert_position();
}
void App::finish_operation() {
    if (!op_pending)
        return;
    op_pending = false;
    if (auth_window && auth_window->shown()) {
        answer_auth(false);
    }
    int exit = operation.exit_code();
    char line[120];
    snprintf(line, sizeof(line), T("\nExit status: %d\n"), exit);
    op_buffer.append(line);
    printf("RELIEFAPK_OPERATION mode=%d exit=%d\n", static_cast<int>(mode), exit);
    fflush(stdout);
    if (exit || operation.truncated()) {
        if (mode == OperationMode::Save)
            repository_finished(repositories, false);
        op_error = true;
        mode = OperationMode::Done;
        op_status->value(T("Operation failed. Review the output below."));
        apply_button->deactivate();
        cancel_button->copy_label(T("Close"));
        cancel_button->activate();
        clear_button->activate();
        set_status(T("Operation failed; remaining changes were not executed."));
        selection_changed();
        return;
    }
    if (mode == OperationMode::Preview) {
        if (++current_group < groups.size()) {
            start_group(true);
            return;
        }
        mode = OperationMode::Await;
        clear_button->deactivate();
        op_status->value(T("Simulation completed. Review the output, then apply."));
        progress->value(100);
        apply_button->activate();
        cancel_button->copy_label(T("Close"));
        cancel_button->activate();
        return;
    }
    if (mode == OperationMode::Recheck) {
        mode = OperationMode::Execute;
        start_group(false);
        return;
    }
    if (mode == OperationMode::Execute) {
        Action completed = groups[current_group];
        for (size_t i = queue.size(); i > 0; --i)
            if (queue[i - 1].action == completed)
                queue.erase(i - 1);
        update_queue();
        if (++current_group < groups.size()) {
            mode = OperationMode::Recheck;
            start_group(true);
            return;
        }
    }
    bool saved = mode == OperationMode::Save;
    mode = OperationMode::Done;
    op_status->value(T("Completed successfully."));
    progress->value(100);
    cancel_button->copy_label(T("Close"));
    cancel_button->activate();
    clear_button->activate();
    selection_changed();
    request_catalog();
    if (saved) {
        repo_dirty = false;
        repository_finished(repositories, true);
    }
}
void App::apply_changes() {
    if (mode != OperationMode::Await)
        return;
    current_group = 0;
    mode = OperationMode::Recheck;
    op_buffer.append(T("\nApplying the reviewed operations...\n"));
    start_group(true);
}
void App::close_operation() {
    if (operation.running()) {
        if (operation.password_requested()) {
            operation.cancel_auth();
            if (auth_window)
                auth_window->hide();
            return;
        }
        if (operation.cancel()) {
            if (auth_window)
                auth_window->hide();
            op_status->value(T("Canceling..."));
        } else
            fl_message("%s", T("apk is applying changes. Wait for it to finish before closing."));
        return;
    }
    if (op_window)
        op_window->hide();
    if (mode == OperationMode::Await)
        mode = OperationMode::Idle;
    selection_changed();
}
void App::open_local() {
    const char *path = fl_file_chooser(T("Install local APK"), "*.apk", nullptr);
    if (path) {
        queue_target(Action::Install, path);
        show_queue();
    }
}
void App::close_app() {
    if (operation.running()) {
        fl_message("%s", T("An operation is still running. Finish or cancel it in the operation window."));
        op_window->show();
        return;
    }
    quitting = true;
    query.cancel();
    detail.cancel();
    if (op_window)
        op_window->hide();
    if (queue_window)
        queue_window->hide();
    if (auth_window)
        auth_window->hide();
    if (settings_window)
        settings_window->hide();
    for (Fl_Window *window = Fl::first_window(); window;) {
        Fl_Window *next = Fl::next_window(window);
        window->hide();
        window = next;
    }
    hide();
}
} // namespace ra
