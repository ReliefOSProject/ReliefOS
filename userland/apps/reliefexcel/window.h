#ifndef RELIEFEXCEL_WINDOW_H
#define RELIEFEXCEL_WINDOW_H
#include "model.h"
#include "document.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Button.H>
namespace rx {
class SheetGrid;
enum class Command {
    New, Open, Save, SaveAs, Import, Export, Template, Quit, Undo, Redo,
    Cut, Copy, Paste, PasteValues, SelectAll, FillDown, FillRight,
    Bold, Italic, Size, TextColor, FillColor, Borders, NoBorders,
    AlignLeft, AlignCenter, AlignRight, Wrap, Merge, Unmerge,
    General, Number, Percent, Currency,
    InsertRow, DeleteRow, InsertColumn, DeleteColumn, AddSheet, DeleteSheet, RenameSheet, MoveLeft, MoveRight,
    AutoSum, Function, Recalculate, Formulas,
    SortAscending, SortDescending, Filter, ClearFilter, Find, Replace,
    FreezeRow, FreezeColumn, FreezeBoth, Unfreeze, Gridlines, Zoom
};
class ExcelWindow : public Fl_Double_Window {
    friend class SheetGrid;
    Fl_Group *ribbon_tabs_ = nullptr, *ribbon_ = nullptr, *formula_group_ = nullptr, *bottom_ = nullptr;
    Fl_Scroll *ribbon_scroll_ = nullptr, *sheet_scroll_ = nullptr;
    Fl_Group *panes_[5]{};
    Fl_Button *tabs_[5]{};
    Fl_Menu_Button *file_ = nullptr;
    Fl_Input *name_ = nullptr, *formula_ = nullptr;
    Fl_Box *status_ = nullptr, *statistics_ = nullptr;
    Fl_Button *cancel_ = nullptr;
    Fl_Choice *size_ = nullptr;
    Array<Fl_Button *> sheet_buttons_;
    Text path_, recovery_path_, recovered_from_, search_;
    int recovery_descriptor_ = -1, recovered_descriptor_ = -1;
    FileStamp stamp_;
    uint64_t saved_revision_ = 0, recovery_revision_ = 0;
    bool busy_ = false, canceled_ = false, updating_ = false, search_case_ = false;
    bool paste_values_ = false;
    unsigned busy_depth_ = 0;
    int progress_percent_ = -1;
    uint64_t progress_tick_ = 0;
    int ribbon_index_ = 0;
    bool confirm_discard();
    void discard_recovery();
    bool choose_path(Text &path, bool save, bool csv = false);
    bool save(bool as_new);
    void open(const char *path, bool csv);
    void build_ribbon();
    void build_sheet_tabs();
    void select_ribbon(int index);
    void function_dialog();
    void data_dialog(bool filter, bool descending);
    void search_dialog(bool replace);
    bool find_next(const char *find, bool match_case);
    void template_book();
    void set_busy(bool busy);
    static bool progress(void *, unsigned, unsigned);
    static void recovery_tick(void *);
    static void dispatch(Fl_Widget *, void *);
    static void ribbon_tab_cb(Fl_Widget *, void *);
    static void sheet_cb(Fl_Widget *, void *);
    static void formula_cb(Fl_Widget *, void *);
    static void name_cb(Fl_Widget *, void *);
    static void close_cb(Fl_Widget *, void *);
public:
    Editor editor;
    SheetGrid *grid = nullptr;
    Clipboard clipboard;
    Text clipboard_text;
    unsigned long clipboard_owner = 0;
    bool show_formulas = false;
    ExcelWindow();
    ~ExcelWindow() override;
    Sheet *sheet() { return editor.book.sheet(editor.book.active_id); }
    const Sheet *sheet() const { return editor.book.sheet(editor.book.active_id); }
    void act(Command command);
    void changed(bool sheet_list = false);
    void selected();
    void message(const char *text);
    bool commit_cell(int row, int col, const char *text, bool literal = false);
    bool commit_formula();
    bool is_busy() const { return busy_; }
    bool own_clipboard() const;
    void paste_text(const char *bytes, size_t length);
    void load_initial(const char *path);
    void offer_recovery();
    void resize(int,int,int,int) override;
    int handle(int event) override;
};
}
#endif
