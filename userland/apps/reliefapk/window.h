#ifndef RELIEFAPK_WINDOW_H
#define RELIEFAPK_WINDOW_H
#include "../fltk_support.h"
#include "model.h"
#include "process.h"
#include "repositories.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Tile.H>
namespace ra {
class App;
class RepositoryDialog;
void repository_finished(RepositoryDialog *, bool);
void destroy_repositories(RepositoryDialog *);
struct QueueEntry {
    Action action = Action::Install;
    Text target;
};
class PackageTable : public Fl_Table_Row {
  public:
    App *app;
    PackageTable(int, int, int, int, App *);
    void draw_cell(TableContext, int, int, int, int, int, int) override;
};
enum class OperationMode { Idle, Preview, Await, Recheck, Execute, Tool, Save, Done };
class App : public Fl_Double_Window {
  public:
    Settings settings;
    AuthMode auth_mode = AuthMode::Sudo;
    Array<Package> packages, installed;
    Array<unsigned> visible;
    Array<QueueEntry> queue;
    Array<Action> groups;
    Process query, detail, operation;
    PackageParser parser, detail_parser;
    bool query_parsing = false, detail_parsing = false, query_pending = false;
    int wanted_filter = 1, query_filter = 1, detail_kind = 0, detail_query_kind = 0, catalog_second = -1;
    Text detail_name, detail_version, initial_path;
    size_t current_group = 0, out_seen = 0, err_seen = 0;
    OperationMode mode = OperationMode::Idle;
    bool op_pending = false, op_error = false, quitting = false, repo_dirty = false;
    bool initial_ca_repair = false;
    unsigned selected_count = 0;
    Fl_Menu_Bar *menu = nullptr;
    Fl_Button *refresh_button = nullptr, *index_button = nullptr, *upgrade_button = nullptr,
              *local_button = nullptr, *queue_button = nullptr;
    Fl_Button *install_button = nullptr, *remove_button = nullptr, *selected_upgrade = nullptr,
              *repair_button = nullptr;
    Fl_Input *search = nullptr;
    Fl_Hold_Browser *sidebar = nullptr;
    PackageTable *table = nullptr;
    Fl_Tile *tile = nullptr;
    Fl_Group *detail_group = nullptr;
    Fl_Tabs *tabs = nullptr;
    Fl_Text_Display *detail_views[4] = {};
    Fl_Text_Buffer detail_buffers[4];
    Fl_Output *status = nullptr;
    Fl_Double_Window *op_window = nullptr;
    Fl_Text_Display *op_view = nullptr;
    Fl_Text_Buffer op_buffer;
    Fl_Output *op_status = nullptr;
    Fl_Progress *progress = nullptr;
    Fl_Button *apply_button = nullptr, *cancel_button = nullptr, *clear_button = nullptr;
    Fl_Double_Window *auth_window = nullptr;
    Fl_Secret_Input *password = nullptr;
    Fl_Output *auth_description = nullptr;
    RepositoryDialog *repositories = nullptr;
    Fl_Double_Window *queue_window = nullptr;
    Fl_Hold_Browser *queue_list = nullptr;
    Fl_Double_Window *settings_window = nullptr;
    Fl_Check_Button *offline = nullptr;
    Fl_Input *timeout_input = nullptr, *age_input = nullptr;
    Fl_Choice *auth_choice = nullptr;
    App(const char *path = nullptr);
    ~App();
    int handle(int event) override;
    void resize(int, int, int, int) override;
    void layout();
    void set_status(const char *);
    void tick();
    void request_catalog();
    void begin_catalog(int filter);
    void filter_rows();
    void selection_changed();
    void request_details();
    const Package *selected() const;
    const Package *installed_package(const char *) const;
    void enqueue(Action);
    void queue_target(Action, const char *);
    void show_queue();
    void update_queue();
    void preview_queue();
    void start_group(bool simulate);
    bool launch(Command &, OperationMode);
    void tool(Action);
    void show_operation(const char *);
    void append_log(const char *, size_t);
    void finish_operation();
    void apply_changes();
    void close_operation();
    void show_auth();
    void answer_auth(bool);
    void show_settings();
    void apply_settings();
    void load_settings();
    void save_settings();
    void show_repositories();
    void save_repositories(Repositories &);
    void open_local();
    void close_app();
};
const char *action_name(Action);
void text_field(Fl_Text_Buffer &, const char *, const char *);
void show_text(const char *, const char *);
} // namespace ra
#endif
