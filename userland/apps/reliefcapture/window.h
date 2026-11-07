#ifndef RELIEFCAPTURE_WINDOW_H
#define RELIEFCAPTURE_WINDOW_H
#include "overlay.h"
#include "job.h"
#include "keyboard.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>
namespace rc {
class CaptureWindow;
class Canvas : public Fl_Widget {
  CaptureWindow &owner_;
  bool drawing_ = false;
  Mark pending_;
  Fl_Image *preview_ = nullptr;
  const uint32_t *cached_source_ = nullptr;
  Rect view() const;
  bool point(int, int, Point &, bool clamp = false) const;
public:
  Canvas(CaptureWindow &);
  ~Canvas() override;
  void draw() override;
  int handle(int) override;
};
class CaptureWindow : public Fl_Double_Window {
  friend class Canvas;
  Document document_;
  Job job_;
  Overlay *overlay_ = nullptr;
  Canvas *canvas_ = nullptr;
  Fl_Scroll *scroll_ = nullptr;
  Fl_Group *tools_ = nullptr;
  Fl_Choice *mode_, *delay_, *tool_, *color_, *line_, *font_, *zoom_;
  Fl_Button *capture_, *copy_, *save_, *undo_, *redo_, *help_;
  Fl_Box *status_, *detail_;
  bool only_select_ = false, closed_ = false, closing_ = false, dirty_ = false;
  bool waiting_ = false, capture_pending_ = false;
  bool queued_capture_ = false;
  unsigned dialog_depth_ = 0;
  EscapeKey escape_key_;
  unsigned active_mode_ = 0;
  double capture_at_ = 0, hidden_at_ = 0;
  char path_[PATH_MAX]{}, saving_path_[PATH_MAX]{};
  void layout();
  void refresh();
  void message(const char *);
  void begin_capture(bool shortcut);
  void complete_capture(Image &&);
  void cancel_capture();
  void capture_failed(const char *);
  void finish_overlay(Rect, bool);
  void copy_image();
  bool save_image();
  void close_window();
  void undo(bool redo);
  void help();
  void tick();
  static void timer(void *);
  bool grab_cancel_key();
  void release_cancel_key();
  static int system_event(void *, void *);
public:
  explicit CaptureWindow(bool only_select);
  ~CaptureWindow() override;
  bool running() const { return !closed_; }
  void request(bool shortcut);
  bool hotkey();
  int handle(int) override;
  void resize(int, int, int, int) override;
};
}
#endif
