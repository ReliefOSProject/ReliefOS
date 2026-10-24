#ifndef RELIEFMATHPP_WINDOW_H
#define RELIEFMATHPP_WINDOW_H
#include "../fltk_support.h"
#ifdef None
#undef None
#endif
#include "../relieftodo/file_io.h"
#include "canvas.h"
#include "document.h"
#include "mesh.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Value_Slider.H>
namespace rmp {
enum class Command {
  New,
  Open,
  Save,
  SaveAs,
  Close,
  Undo,
  Redo,
  Duplicate,
  Delete,
  Toggle,
  Apply,
  Reset,
  ZoomIn,
  ZoomOut,
  Fit,
  Grid,
  Axes,
  Wire,
  Trace,
  ExportSvg,
  ExportPng,
  Table,
  Calculate,
  Surface,
  Measure,
  Vectors,
  Help,
  Example2,
  Example3,
  ExampleTorus,
  ExamplePolar,
  PickPoint,
  PickSegment,
  PickCircle,
  PickTriangle,
  PickVector,
  CancelPick
};
class MathWindow : public Fl_Double_Window {
  Fl_Group *top_ = nullptr, *quick_ = nullptr, *body_ = nullptr,
           *left_ = nullptr, *right_ = nullptr, *viewbar_ = nullptr,
           *params_ = nullptr, *bottom_ = nullptr;
  Fl_Tabs *tools_ = nullptr, *views_ = nullptr;
  Fl_Group *toolpages_[3]{}, *viewpages_[2]{};
  Fl_Hold_Browser *list_ = nullptr;
  Fl_Scroll *properties_ = nullptr;
  Fl_Input *name_ = nullptr, *formula_[3]{}, *coords_ = nullptr, *ranges_[2]{},
           *parameter_[4]{};
  Fl_Choice *type_ = nullptr, *color_ = nullptr, *quality_ = nullptr;
  Fl_Check_Button *visible_ = nullptr, *grid_ = nullptr, *axes_ = nullptr,
                  *wire_ = nullptr, *trace_ = nullptr;
  Fl_Box *range_labels_[2]{}, *color_label_ = nullptr,
                              *quality_label_ = nullptr, *tip_ = nullptr;
  Fl_Box *formula_labels_[3]{}, *coord_label_ = nullptr, *hint_ = nullptr,
                                *status_ = nullptr, *counts_ = nullptr;
  Fl_Value_Slider *sliders_[4]{};
  Fl_Button *apply_ = nullptr;
  uint64_t active_ = 0;
  bool loading_ = false, dirty_ = false, fields_dirty_ = false,
       sampling_ = false, trace_mode_ = false;
  size_t sample_index_ = 0;
  Sampler sampler_;
  History history_;
  Text path_, pending_path_, notice_;
  Bytes pending_bytes_;
  rt::IoJob io_;
  rt::FileStamp stamp_;
  enum class Operation {
    Idle,
    Open,
    Save,
    Export
  } operation_ = Operation::Idle;
  int stage_ = 0;
  bool closing_ = false;
  Kind pick_kind_ = Kind::Count;
  Vec3 picks_[3]{};
  int pick_count_ = 0;
  void build_tools();
  void layout();
  void layout_properties();
  void refill();
  void refresh();
  bool apply_fields();
  bool checkpoint();
  bool finish_dirty();
  void file_action(Command);
  void poll_io();
  void poll_sampling();
  void start_write(const char *, bool export_file);
  void export_action(Command);
  static void tick(void *);
  static void click(Fl_Widget *, void *);
  static void edit(Fl_Widget *, void *);
  static void choose(Fl_Widget *, void *);
  static void parameter_changed(Fl_Widget *, void *);

public:
  Document document;
  Array<Mesh> meshes;
  Canvas *canvas[2]{};
  MathWindow();
  ~MathWindow();
  void run(Command);
  void add_object(Kind);
  void load_initial(const char *);
  void resample();
  void invalidate();
  void changed(bool rebuild = true);
  void update_status(const char *text = nullptr);
  void select_tab(int);
  int tab() const { return document.view.tab; }
  void view_changed();
  void pointer(double, double);
  void picked(Vec3);
  bool picking() const { return pick_kind_ != Kind::Count; }
  void calculate_dialog();
  void surface_dialog();
  void table_dialog();
  void vectors_dialog();
  void help_dialog();
  Object *current() { return rmp::find(document, active_); }
  void resize(int, int, int, int) override;
  int handle(int) override;
};
} // namespace rmp
#endif
