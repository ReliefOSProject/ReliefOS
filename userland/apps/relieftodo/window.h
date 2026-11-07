#ifndef RELIEFTODO_WINDOW_H
#define RELIEFTODO_WINDOW_H
#include "file_io.h"
#include "../fltk_support.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Scroll.H>
namespace rt {
enum class Command {
    Idle, New, Open, Save, SaveAs, ExportCsv, ExportMarkdown, Example, Close,
    Add, Duplicate, Delete, Done, Restore, Toggle, Apply, Revert, Undo, Redo,
    Today, Tomorrow, NextWeek, ClearDate, Low, Normal, High, Urgent,
    MoveUp, MoveDown, Project, Tags, Rename, SelectAll, ClearSelection,
    ClearFilters, Statistics, Help
};
class TodoWindow;
class TaskTable : public Fl_Table_Row {
    TodoWindow *owner_;
    void draw_cell(TableContext,int,int,int,int,int,int) override;
    int handle(int) override;
public:
    TaskTable(int,int,int,int,TodoWindow *);
    void columns();
};
class TodoWindow : public Fl_Double_Window {
    friend class TaskTable;
    Document document_, saved_;
    History history_;
    Array<size_t> visible_;
    Array<TaskId> selected_;
    Array<Text> projects_;
    TaskId active_=0;
    Filter filter_=Filter::All;
    SortMode sort_=SortMode::Manual;
    Text query_, project_, path_, pending_path_;
    FileStamp stamp_, pending_stamp_;
    IoJob io_;
    Bytes pending_bytes_;
    Command pending_kind_=Command::Idle, after_save_=Command::Idle;
    bool loading_=false, fields_dirty_=false, force_dirty_=false;
    int tab_=0;
    Fl_Group *top_=nullptr, *ribbon_=nullptr, *pages_[3]{}, *body_=nullptr, *left_=nullptr, *center_=nullptr, *details_=nullptr, *bottom_=nullptr;
    Fl_Scroll *ribbon_scroll_=nullptr;
    Fl_Button *tabs_[3]{}, *undo_=nullptr, *redo_=nullptr, *apply_=nullptr, *revert_=nullptr;
    Fl_Box *list_heading_=nullptr, *detail_heading_=nullptr, *status_=nullptr, *summary_=nullptr, *meta_=nullptr;
    Fl_Input *search_=nullptr, *title_=nullptr, *project_input_=nullptr, *due_=nullptr, *tags_=nullptr;
    Fl_Hold_Browser *filters_=nullptr, *project_list_=nullptr;
    Fl_Choice *sort_choice_=nullptr, *priority_=nullptr;
    Fl_Check_Button *completed_=nullptr;
    Fl_Text_Buffer notes_;
    Fl_Text_Editor *notes_editor_=nullptr;
    Fl_Box *labels_[7]{};
    Fl_Button *date_buttons_[3]{};
    TaskTable *table_=nullptr;
    static void clicked(Fl_Widget *,void *);
    static void edited(Fl_Widget *,void *);
    static void notes_changed(int,int,int,int,const char *,void *);
    static void tick(void *);
    static void search_tick(void *);
    void build_ribbon();
    void show_tab(int);
    void layout();
    void refill(bool projects=true);
    void refresh_editor();
    void table_selection(int);
    void select_relative(int);
    bool apply_editor(bool refresh=true);
    bool begin_change();
    void finish_change(bool, const char *);
    bool dirty() const;
    void update_status(const char *message=nullptr);
    void choose_filter();
    void choose_project();
    void choose_sort();
    void poll_io();
    void lock_document(bool);
    bool confirm_replace(Command);
    bool choose_path(Text &,bool,const char *);
    void save(bool);
    void start_write(bool probe);
    void export_file(Command);
    void reset_document(bool example);
    void statistics();
public:
    TodoWindow();
    ~TodoWindow() override;
    void run(Command);
    void load_initial(const char *);
    void resize(int,int,int,int) override;
    int handle(int) override;
};
}
#endif
