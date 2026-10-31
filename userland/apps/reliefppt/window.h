#ifndef RELIEFPPT_WINDOW_H
#define RELIEFPPT_WINDOW_H
#include "document.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Scroll.H>
namespace rp {
class Canvas;
class Thumbnails;
enum class Command {
    New,
    Open,
    Save,
    SaveAs,
    ExportPng,
    ExportHtml,
    Example,
    Quit,
    About,
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    Delete,
    Duplicate,
    SelectAll,
    NewSlide,
    CopySlide,
    DeleteSlide,
    SlideUp,
    SlideDown,
    TextBox,
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Image,
    Font,
    Size,
    Bold,
    Italic,
    Underline,
    TextColor,
    FillColor,
    LineColor,
    TextLeft,
    TextCenter,
    TextRight,
    Bullets,
    Properties,
    AlignLeft,
    AlignCenter,
    AlignRight,
    AlignTop,
    AlignMiddle,
    AlignBottom,
    Top,
    Bottom,
    Raise,
    Lower,
    Wide,
    Standard,
    Background,
    ThemeBlue,
    ThemeGreen,
    ThemeLight,
    Start,
    StartCurrent,
    Grid,
    Snap,
    Notes,
    Fit,
    Zoom
};
class PptWindow : public Fl_Double_Window {
    friend class Canvas;
    friend class Thumbnails;
    Fl_Group *top_ = nullptr, *ribbon_ = nullptr, *notes_group_ = nullptr;
    Fl_Scroll *ribbon_scroll_ = nullptr;
    Fl_Group *panes_[4]{};
    Fl_Button *tabs_[4]{}, *quick_[3]{}, *toggles_[3]{};
    Fl_Choice *font_ = nullptr, *layout_ = nullptr, *zoom_ = nullptr;
    Fl_Input_Choice *size_ = nullptr;
    Fl_Check_Button *grid_ = nullptr, *snap_ = nullptr, *notes_toggle_ = nullptr;
    Fl_Box *status_ = nullptr, *summary_ = nullptr;
    Fl_Multiline_Input *notes_ = nullptr;
    Text path_, recovery_path_, recovered_from_, clipboard_text_;
    Deck clipboard_;
    unsigned long clipboard_owner_ = 0;
    FileStamp stamp_;
    uint64_t saved_revision_ = 0, recovery_revision_ = 0;
    int recovery_fd_ = -1, recovered_fd_ = -1;
    uint64_t pending_epoch_ = 0, recovery_epoch_ = 0;
    bool busy_ = false, updating_ = false, notes_visible_ = false;
    int tab_ = 0;
    size_t notes_page_ = SIZE_MAX;
    void build_ribbon();
    void show_tab(int);
    void title();
    bool dirty() const;
    bool confirm_discard();
    bool save(bool as_new);
    bool choose_path(Text &, bool save, const char *extension);
    void export_file(bool html);
    void open_file(const char *);
    void discard_recovery();
    void insert(Kind);
    void object_dialog();
    void color_dialog(Command);
    void theme(unsigned);
    bool style_selection(Command);
    void copy_selection(bool cut);
    void paste();
    static void dispatch(Fl_Widget *, void *);
    static void tab_cb(Fl_Widget *, void *);
    static void close_cb(Fl_Widget *, void *);
    static void recovery_tick(void *);

  public:
    Editor editor;
    Canvas *canvas = nullptr;
    Thumbnails *thumbs = nullptr;
    PptWindow();
    ~PptWindow() override;
    void act(Command);
    void changed();
    void selected();
    void message(const char *);
    bool commit_inputs();
    void change_slide(size_t);
    void pending_input();
    void load_initial(const char *);
    void offer_recovery();
    void slideshow(bool from_current);
    void receive_text(const char *, size_t);
    bool is_busy() const { return busy_; }
    bool show_grid() const { return grid_ && grid_->value(); }
    bool snap_grid() const { return snap_ && snap_->value(); }
    void resize(int, int, int, int) override;
    int handle(int) override;
};
} // namespace rp
#endif
