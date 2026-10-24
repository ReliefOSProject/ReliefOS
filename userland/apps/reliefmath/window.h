#ifndef RELIEFMATH_WINDOW_H
#define RELIEFMATH_WINDOW_H
#include "../fltk_support.h"
#ifdef None
#undef None
#endif
#include "model.h"
#include "history.h"
#include "editor_core.h"
#include "render_types.h"
#include "ui_layout.h"
#include "preview.h"
#include "editor_view.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Menu_Button.H>
namespace rm {
struct UiGuard {int &depth;explicit UiGuard(int &value):depth(value){++depth;}~UiGuard(){--depth;}};
class RenderClient;
class IoJob;
class ExportQueue;
enum class Command : int {
    New,Open,Save,SaveAs,Close,Undo,Redo,Cut,Copy,Paste,SelectAll,Find,Replace,Complete,
    Add,Duplicate,Rename,Delete,MoveUp,MoveDown,Symbols,Templates,Favorites,AddFavorite,
    Fraction,Root,Scripts,Integral,Sum,Matrix,Cases,Aligned,Text,Color,Padding,
    ExportSvg,ExportPng,ExportLatex,ExportSelected,ExportAll,CopyImage,CancelJob,
    ZoomIn,ZoomOut,Fit,ShowList,ShowPreview,Help,Example,ErrorLocation
};
class MathWindow : public Fl_Double_Window {
    Fl_Group *top_=nullptr,*ribbon_=nullptr,*ribbon_pages_[5]{},*list_panel_=nullptr,*edit_panel_=nullptr,*preview_panel_=nullptr,*bottom_=nullptr;
    Fl_Scroll *ribbon_scroll_=nullptr;
    Fl_Tile *tile_=nullptr;
    Fl_Multi_Browser *list_=nullptr;
    Fl_Input *filter_=nullptr;
    Fl_Text_Buffer source_,styles_;
    Fl_Box *status_=nullptr,*summary_=nullptr,*render_label_=nullptr;
    Fl_Button *error_button_=nullptr,*tabs_[5]{},*undo_=nullptr,*redo_=nullptr;
    Fl_Choice *size_choice_=nullptr,*display_choice_=nullptr;
    Array<FormulaId> visible_ids_;
    FormulaId active_=0;
    bool loading_=false,show_list_=true,show_preview_=true,render_pending_=false,stale_=false,document_locked_=false;
    double last_edit_=0,zoom_=1;
    uint64_t request_=0,session_=1;
    int interaction_depth_=0;
    Error render_error_;
    Snapshot submitted_,displayed_;
    RenderClient *renderer_=nullptr;
    IoJob *io_=nullptr;
    ExportQueue *exports_=nullptr;
    Text path_,notice_;
    struct FileState;
    FileState *files_=nullptr;
    void build_ribbon();
    void select_formula(FormulaId);
    void refill_list();
    void update_status(const char *message=nullptr);
    void restyle();
    void refresh_source(bool reset_stops=true);
    void flush_edit();
    void schedule_render();
    void layout_panels();
    bool finish_dirty();
    void file_action(Command);
    void start_recovery_load(uint64_t);
    void start_saved_recovery_cleanup();
    void export_action(Command);
    void recovery_tick();
    void finish_file_job();
    void init_files();
    void destroy_files();
    void export_progress();
    void commit_core(bool select_stop);
    static void tick(void *);
    static void recovery_timer(void *);
    static void source_changed(int,int,int,int,const char*,void*);
    static void clicked(Fl_Widget*,void*);
    static void selected(Fl_Widget*,void*);
    static void style_changed(Fl_Widget*,void*);
public:
    Document document;
    History history;
    EditorCore core;
    SourceEditor *editor=nullptr;
    Preview *preview=nullptr;
    MathWindow();
    ~MathWindow();
    void run(Command);
    void load_initial(const char *);
    void offer_recovery();
    void show_tab(int);
    void insert_tool(Tool);
    void insert_source(const char *);
    void symbol_dialog(bool templates=false,bool favorites=false);
    void find_dialog(bool replace);
    void complete();
    void structure_dialog(Tool);
    void help_dialog();
    void favorite_dialog();
    bool placeholder(bool reverse);
    void poll();
    Formula *current();
    void resize(int,int,int,int) override;
    int handle(int) override;
};
}
#endif
