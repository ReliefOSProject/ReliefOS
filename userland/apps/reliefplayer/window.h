#ifndef RELIEFPLAYER_WINDOW_H
#define RELIEFPLAYER_WINDOW_H
#include "../fltk_support.h"
#include "session.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Value_Slider.H>
namespace rp {
class ProgressSlider : public Fl_Slider {
public:
  bool dragging = false;
  ProgressSlider() : Fl_Slider(0, 0, 1, 1) {}
  int handle(int event) override;
};
class PlayerWindow : public Fl_Double_Window {
  Session session_;
  char path_[4096]{};
  bool closing_ = false;
  Fl_Button *open_, *help_, *play_, *stop_, *back_, *forward_;
  Fl_Box *panel_, *title_, *detail_, *elapsed_, *duration_, *status_;
  Fl_Check_Button *mute_;
  ProgressSlider *seek_;
  Fl_Value_Slider *volume_;
  void layout();
  void update();
  void toggle();
  void skip(int seconds);
  void choose();
  void close_player();
  static void tick(void *);
  static void action(Fl_Widget *, void *);

public:
  PlayerWindow();
  ~PlayerWindow() override;
  void open_path(const char *path);
  bool hotkey();
  void resize(int x, int y, int w, int h) override;
  int handle(int event) override;
};
} // namespace rp
#endif
