#ifndef RELIEFWORD_WINDOW_H
#define RELIEFWORD_WINDOW_H
#include "document.h"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Menu_Button.H>
namespace rw {
class PageView;
enum class Command {
    New,Open,Save,SaveAs,ImportText,ImportHtml,ExportText,ExportHtml,Example,Quit,
    Undo,Redo,Cut,Copy,Paste,PastePlain,SelectAll,Find,Replace,FindNext,ReplaceOne,ReplaceAll,
    Bold,Italic,Underline,Strike,Font,Size,Color,Highlight,ClearFormat,Preset,
    AlignLeft,AlignCenter,AlignRight,AlignJustify,Bullets,Numbering,Indent,Outdent,
    Paragraph,PageSetup,PageBreak,Date,Ruler,Zoom,ZoomIn,ZoomOut,Zoom100,FitWidth,Count,About
};
class WordWindow:public Fl_Double_Window {
    friend class PageView;
    Fl_Group *top_=nullptr,*ribbon_=nullptr,*bottom_=nullptr;
    Fl_Scroll *ribbon_scroll_=nullptr;
    Fl_Group *panes_[3]{};
    Fl_Button *tabs_[3]{},*quick_save_=nullptr,*undo_=nullptr,*redo_=nullptr,*format_[4]{};
    Fl_Choice *font_=nullptr,*preset_=nullptr,*zoom_=nullptr;
    Fl_Input_Choice *size_=nullptr;
    Fl_Box *status_=nullptr,*summary_=nullptr;
    Fl_Window *search_window_=nullptr;
    Fl_Input *find_=nullptr,*replacement_=nullptr;
    Fl_Check_Button *match_case_=nullptr,*ruler_button_=nullptr;
    Document clipboard_;
    Text clipboard_text_,path_,import_path_,recovery_path_,recovered_from_;
    unsigned long clipboard_owner_=0;
    FileStamp stamp_,import_stamp_;
    uint64_t saved_revision_=0,recovery_revision_=0,statistics_revision_=0;
    Statistics statistics_;
    int recovery_descriptor_=-1,recovered_descriptor_=-1;
    bool updating_=false,busy_=false,paste_plain_=false;
    int ribbon_index_=0;
    void build_ribbon(); void show_tab(int tab);
    bool confirm_discard(); bool save(bool as_new);
    bool choose_path(Text &,bool save,FileFormat format);
    void open_file(const char *,FileFormat format);
    void export_file(FileFormat format);
    void discard_recovery();
    void page_dialog(); void paragraph_dialog(); void search_dialog(bool replace);
    void find_next(); void replace_one(); void replace_all();
    void example_document();
    static void dispatch(Fl_Widget *,void *);
    static void tab_cb(Fl_Widget *,void *);
    static void close_cb(Fl_Widget *,void *);
    static void recovery_tick(void *);
public:
    Editor editor;
    PageView *view=nullptr;
    WordWindow(); ~WordWindow() override;
    void act(Command command);
    void changed(); void selected();
    void message(const char *text);
    void paste_text(const char *bytes,size_t length,bool plain=false);
    void load_initial(const char *path); void offer_recovery();
    bool is_busy() const { return busy_; }
    void resize(int,int,int,int) override;
    int handle(int event) override;
};
}
#endif
