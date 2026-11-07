#ifndef RELIEFDIAGRAM_WINDOW_H
#define RELIEFDIAGRAM_WINDOW_H
#include "../fltk_support.h"
#ifdef None
#undef None
#endif
#include "../relieftodo/file_io.h"
#include "document.h"
#include "export.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Value_Input.H>
namespace rd {
class Canvas;
enum class Command {
  New,
  Open,
  Save,
  SaveAs,
  Close,
  Undo,
  Redo,
  Copy,
  Paste,
  Duplicate,
  Delete,
  Child,
  Sibling,
  Fold,
  MindLayout,
  TreeLayout,
  FlowLayout,
  Apply,
  Fit,
  ZoomIn,
  ZoomOut,
  ActualSize,
  Grid,
  Snap,
  SelectAll,
  Front,
  Back,
  AlignLeft,
  AlignTop,
  AlignCenter,
  AlignMiddle,
  DistributeH,
  DistributeV,
  ExportSvg,
  ExportPng,
  ExportOutline,
  TemplateMind,
  TemplateFlow,
  TemplateLogic,
  Title,
  Help,
  SelectMode,
  ConnectMode,
  PanMode
};
class DiagramWindow : public Fl_Double_Window {
  Fl_Group *menus_ = nullptr, *quick_ = nullptr, *body_ = nullptr,
           *left_ = nullptr, *right_ = nullptr, *viewbar_ = nullptr,
           *bottom_ = nullptr;
  Fl_Tabs *tools_ = nullptr;
  Fl_Group *toolpages_[3]{}, *node_fields_ = nullptr, *edge_fields_ = nullptr;
  Fl_Hold_Browser *shapes_ = nullptr, *objects_ = nullptr;
  Array<uint64_t> rows_;
  Fl_Scroll *properties_ = nullptr;
  Fl_Multiline_Input *text_ = nullptr, *note_ = nullptr;
  Fl_Choice *shape_ = nullptr, *route_ = nullptr, *arrows_ = nullptr,
            *mode_ = nullptr;
  Fl_Value_Input *numbers_[4]{}, *font_ = nullptr, *width_ = nullptr;
  Fl_Check_Button *bold_ = nullptr, *grid_ = nullptr, *snap_ = nullptr;
  Fl_Button *colors_[3]{}, *apply_ = nullptr;
  Fl_Box *status_ = nullptr, *counts_ = nullptr, *property_title_ = nullptr,
         *title_ = nullptr;
  uint32_t color_values_[3]{};
  bool fields_dirty_ = false, loading_ = false, dirty_ = false,
       closing_ = false;
  History history_;
  Document clipboard_;
  Text path_, pending_path_, notice_;
  Bytes pending_bytes_;
  rt::IoJob io_;
  rt::FileStamp stamp_;
  PngJob png_;
  enum class Operation {
    Idle,
    Open,
    Save,
    Export,
    Png
  } operation_ = Operation::Idle;
  int stage_ = 0;
  Command resume_ = Command::Close;
  bool resume_pending_ = false;
  void build_tools();
  void layout();
  void refill();
  void refresh();
  bool finish_dirty();
  void file_action(Command);
  void start_write(const char *, bool);
  void poll_io();
  void export_action(Command);
  void busy_ui(bool);
  void copy_selection();
  void paste_selection();
  static void tick(void *);
  static void click(Fl_Widget *, void *);
  static void edit(Fl_Widget *, void *);
  static void choose(Fl_Widget *, void *);
  static void color_pick(Fl_Widget *, void *);

public:
  Document document;
  Array<uint64_t> selection;
  Canvas *canvas = nullptr;
  DiagramWindow();
  ~DiagramWindow();
  void run(Command);
  void load_initial(const char *);
  bool apply_fields();
  bool commit(Document &&);
  bool begin_gesture();
  void finish_gesture();
  bool select(uint64_t, bool toggle = false);
  bool selected(uint64_t) const;
  void selection_changed();
  void changed(bool rebuild_list = true);
  void update_status(const char *message = nullptr);
  void add_shape(Shape);
  void edit_label();
  void connect(uint64_t, uint64_t);
  void nudge(double, double);
  bool busy() const { return operation_ != Operation::Idle; }
  int mode() const { return mode_->value(); }
  void resize(int, int, int, int) override;
  int handle(int) override;
};
} // namespace rd
#endif
