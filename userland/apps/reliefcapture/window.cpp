#include "window.h"
#include "render.h"
#include <FL/x.H>
#include <math.h>
#include <stdio.h>
#include <time.h>
namespace rc {
namespace {
const unsigned colors[] = {0xe53935, 0xff8f00, 0xffd600, 0x2e9e48, 0x1976d2, 0xffffff, 0x111111};
const unsigned widths[] = {2, 4, 8, 16};
const unsigned fonts[] = {16, 24, 32, 48};
void enable(Fl_Widget *w, bool active) { if (active) w->activate(); else w->deactivate(); }
struct DialogGuard {
  unsigned &depth;
  explicit DialogGuard(unsigned &value) : depth(value) { ++depth; }
  ~DialogGuard() { --depth; }
};
}
Canvas::Canvas(CaptureWindow &owner) : Fl_Widget(0, 0, 1, 1), owner_(owner) {}
Canvas::~Canvas() { delete preview_; }
Rect Canvas::view() const {
  const auto &im = owner_.document_.image();
  return fit_rect(im.width, im.height, {x() + 16, y() + 16, w() - 32, h() - 32}, owner_.zoom_->value() == 1);
}
bool Canvas::point(int x, int y, Point &out, bool clamp) const {
  Rect r = view(); if (!r.valid()) return false;
  if (clamp) {
    if (x < r.x) x = r.x;
    if (y < r.y) y = r.y;
    if (x >= r.x + r.w) x = r.x + r.w - 1;
    if (y >= r.y + r.h) y = r.y + r.h - 1;
  }
  const auto &im = owner_.document_.image();
  return image_point(r, im.width, im.height, x, y, out);
}
void Canvas::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_color(FL_BACKGROUND2_COLOR); fl_rectf(x(), y(), w(), h());
  if (owner_.document_.image().valid()) {
    const auto &im = owner_.document_.image(); Rect r = view();
    if (!preview_ || cached_source_ != im.pixels.data() || preview_->w() != r.w || preview_->h() != r.h) {
      delete preview_; preview_ = nullptr;
      Array<unsigned char> rgb;
      if (rgb.resize(im.pixels.size() * 3)) {
        for (size_t n = 0; n < im.pixels.size(); ++n) {
          rgb[n * 3] = im.pixels[n] >> 16; rgb[n * 3 + 1] = im.pixels[n] >> 8; rgb[n * 3 + 2] = im.pixels[n];
        }
        Fl_RGB_Image source(rgb.data(), im.width, im.height, 3);
        preview_ = source.copy(r.w, r.h); cached_source_ = im.pixels.data();
      }
    }
    if (preview_) {
      fl_push_clip(r.x, r.y, r.w, r.h); preview_->draw(r.x, r.y);
      for (size_t n = 0; n < owner_.document_.visible(); ++n) draw_mark(owner_.document_.marks()[n], r, im.width, im.height);
      if (drawing_) draw_mark(pending_, r, im.width, im.height);
      fl_pop_clip();
    } else draw_document(owner_.document_, r, drawing_ ? &pending_ : nullptr);
  }
  else {
    fl_color(FL_FOREGROUND_COLOR); fl_font(FL_HELVETICA, 15);
    fl_draw(T("No screenshot. Click New capture or press Win+Shift+S."),
            x() + 24, y() + h() / 2 - 24, w() - 48, 48, FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  }
  fl_pop_clip();
}
int Canvas::handle(int event) {
  if (owner_.job_.busy() || owner_.waiting_ || owner_.capture_pending_) return 0;
  if (event == FL_ENTER) { fl_cursor(FL_CURSOR_CROSS); return 1; }
  if (event == FL_LEAVE) { fl_cursor(FL_CURSOR_DEFAULT); return 1; }
  Point p;
  if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && point(Fl::event_x(), Fl::event_y(), p)) {
    pending_ = Mark{}; pending_.tool = Tool(unsigned(owner_.tool_->value()));
    pending_.a = pending_.b = p; pending_.color = colors[owner_.color_->value()];
    pending_.width = widths[owner_.line_->value()]; pending_.font = fonts[owner_.font_->value()];
    if (pending_.tool == Tool::Label) {
      DialogGuard dialog(owner_.dialog_depth_);
      const char *text = fl_input("%s", "", T("Annotation text"));
      if (!text || !*text) return 1;
      if (strlen(text) > 1024 || !valid_utf8(text, strlen(text)) || !pending_.text.set(text) ||
          !owner_.document_.add(static_cast<Mark &&>(pending_))) {
        owner_.message(T("Could not add the annotation; check its length or the annotation limit")); return 1;
      }
      owner_.dirty_ = true; owner_.refresh();
      printf("[reliefcapture.elf] annotation count=%zu\n", owner_.document_.visible()); fflush(stdout); return 1;
    }
    if (pending_.tool == Tool::Pen && !pending_.points.push(p)) {
      owner_.message(T("Not enough memory")); return 1;
    }
    drawing_ = true; redraw(); return 1;
  }
  if ((event == FL_DRAG || event == FL_RELEASE) && drawing_) {
    if (point(Fl::event_x(), Fl::event_y(), p, true)) {
      pending_.b = p;
      if (pending_.tool == Tool::Pen &&
          (pending_.points.empty() || pending_.points[pending_.points.size() - 1].x != p.x ||
           pending_.points[pending_.points.size() - 1].y != p.y)) {
        if (pending_.points.size() < MaxPoints) {
          if (!pending_.points.push(p)) owner_.message(T("Not enough memory"));
        } else owner_.message(T("The pen stroke reached its point limit; start a new stroke"));
      }
    }
    if (event == FL_RELEASE) {
      drawing_ = false;
      if (owner_.document_.add(static_cast<Mark &&>(pending_))) {
        owner_.dirty_ = true;
        printf("[reliefcapture.elf] annotation count=%zu\n", owner_.document_.visible()); fflush(stdout);
      }
      else if (pending_.a.x != pending_.b.x || pending_.a.y != pending_.b.y)
        owner_.message(T("Could not add the annotation; check its length or the annotation limit"));
      pending_ = Mark{}; owner_.refresh();
    }
    redraw(); return 1;
  }
  return Fl_Widget::handle(event);
}
CaptureWindow::CaptureWindow(bool only_select) : Fl_Double_Window(1000, 660, "ReliefCapture"), only_select_(only_select) {
  begin();
  capture_ = new Fl_Button(12, 20, 116, 32, T("New capture"));
  capture_->copy_tooltip(T("New capture (Ctrl+N); Win+Shift+S selects a region"));
  capture_->callback([](Fl_Widget *, void *p) {
    auto *s = static_cast<CaptureWindow *>(p);
    if (s->waiting_ || s->capture_pending_ || s->job_.kind() == Job::Kind::Capture) s->cancel_capture(); else s->begin_capture(false);
  }, this);
  mode_ = new Fl_Choice(142, 20, 122, 32, T("Capture mode")); mode_->align(FL_ALIGN_TOP_LEFT);
  mode_->add(T("Region")); mode_->add(T("Window")); mode_->add(T("Full screen")); mode_->value(0);
  delay_ = new Fl_Choice(278, 20, 110, 32, T("Delay")); delay_->align(FL_ALIGN_TOP_LEFT);
  delay_->add(T("No delay")); delay_->add(T("3 seconds")); delay_->add(T("5 seconds")); delay_->add(T("10 seconds")); delay_->value(0);
  copy_ = new Fl_Button(0, 20, 108, 32, T("Copy image"));
  copy_->callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->copy_image(); }, this);
  save_ = new Fl_Button(0, 20, 108, 32, T("Save PNG..."));
  save_->callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->save_image(); }, this);
  help_ = new Fl_Button(0, 20, 56, 32, T("Help"));
  help_->callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->help(); }, this);
  tools_ = new Fl_Group(12, 60, w() - 24, 56); tools_->begin();
  tool_ = new Fl_Choice(12, 78, 126, 30, T("Annotation")); tool_->align(FL_ALIGN_TOP_LEFT);
  tool_->add(T("Pen")); tool_->add(T("Arrow")); tool_->add(T("Rectangle")); tool_->add(T("Text")); tool_->value(1);
  color_ = new Fl_Choice(150, 78, 106, 30, T("Color")); color_->align(FL_ALIGN_TOP_LEFT);
  const char *color_names[] = {T("Red"), T("Orange"), T("Yellow"), T("Green"), T("Blue"), T("White"), T("Black")};
  for (const char *s : color_names) color_->add(s);
  color_->value(0);
  line_ = new Fl_Choice(268, 78, 76, 30, T("Line width")); line_->align(FL_ALIGN_TOP_LEFT);
  line_->add("2 px"); line_->add("4 px"); line_->add("8 px"); line_->add("16 px"); line_->value(1);
  font_ = new Fl_Choice(356, 78, 76, 30, T("Text size")); font_->align(FL_ALIGN_TOP_LEFT);
  font_->add("16 px"); font_->add("24 px"); font_->add("32 px"); font_->add("48 px"); font_->value(1);
  undo_ = new Fl_Button(448, 78, 74, 30, T("Undo"));
  undo_->callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->undo(false); }, this);
  redo_ = new Fl_Button(534, 78, 74, 30, T("Redo"));
  redo_->callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->undo(true); }, this);
  zoom_ = new Fl_Choice(0, 78, 120, 30, T("Zoom")); zoom_->align(FL_ALIGN_TOP_LEFT);
  zoom_->add(T("Fit to window")); zoom_->add("100%"); zoom_->value(0);
  zoom_->callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->layout(); }, this);
  tools_->end();
  scroll_ = new Fl_Scroll(12, 120, w() - 24, h() - 162); scroll_->box(FL_DOWN_BOX); scroll_->begin();
  canvas_ = new Canvas(*this); scroll_->end();
  status_ = new Fl_Box(12, h() - 34, 600, 26, T("Ready to capture")); status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  detail_ = new Fl_Box(0, h() - 34, 320, 26); detail_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  end(); size_range(880, 480); callback([](Fl_Widget *, void *p) { static_cast<CaptureWindow *>(p)->close_window(); }, this);
  layout(); refresh(); reliefos_fltk::style(this); Fl::add_timeout(.025, timer, this);
  Fl::add_system_handler(system_event, this);
}
CaptureWindow::~CaptureWindow() {
  Fl::remove_timeout(timer, this);
  release_cancel_key(); Fl::remove_system_handler(system_event);
  if (overlay_) { overlay_->dismiss(); delete overlay_; }
}
void CaptureWindow::layout() {
  copy_->position(w() - 312, 20); save_->position(w() - 192, 20); help_->position(w() - 72, 20);
  tools_->resize(12, 60, w() - 24, 56); zoom_->position(w() - 132, 78);
  scroll_->resize(12, 120, w() - 24, h() - 162);
  const auto &im = document_.image();
  int cw = scroll_->w() - 20, ch = scroll_->h() - 20;
  if (zoom_->value() == 1 && im.valid()) {
    if (cw < im.width + 32) cw = im.width + 32;
    if (ch < im.height + 32) ch = im.height + 32;
  }
  scroll_->scroll_to(0, 0); canvas_->resize(scroll_->x() + 2, scroll_->y() + 2, cw, ch);
  status_->resize(12, h() - 34, w() - 350, 26); detail_->resize(w() - 330, h() - 34, 318, 26);
  redraw();
}
void CaptureWindow::resize(int x, int y, int width, int height) { Fl_Double_Window::resize(x, y, width, height); layout(); }
void CaptureWindow::message(const char *text) { status_->copy_label(text); status_->redraw(); }
void CaptureWindow::refresh() {
  bool idle = !job_.busy() && !waiting_ && !capture_pending_;
  bool have = document_.image().valid();
  enable(copy_, idle && have); enable(save_, idle && have); enable(tools_, idle && have);
  enable(undo_, idle && document_.can_undo()); enable(redo_, idle && document_.can_redo());
  enable(capture_, job_.kind() != Job::Kind::Save);
  capture_->copy_label(waiting_ || capture_pending_ || job_.kind() == Job::Kind::Capture ? T("Cancel") : T("New capture"));
  char detail[160];
  if (have) snprintf(detail, sizeof(detail), "%d × %d px · %zu / 512 · %s", document_.image().width,
                     document_.image().height, document_.visible(), dirty_ ? T("Not saved") : T("Saved"));
  else snprintf(detail, sizeof(detail), "%s", T("No screenshot"));
  detail_->copy_label(detail);
  char title[PATH_MAX + 64]; snprintf(title, sizeof(title), "%sReliefCapture%s%s", dirty_ ? "* " : "", path_[0] ? " — " : "", path_);
  copy_label(title); canvas_->redraw();
}
void CaptureWindow::begin_capture(bool shortcut) {
  if (overlay_ || waiting_ || capture_pending_) return;
  if (job_.busy()) { message(T("Wait for the current screenshot operation to finish")); return; }
  DialogGuard dialog(dialog_depth_);
  if (dirty_ && document_.visible() && fl_choice("%s", T("Cancel"), T("Discard"), nullptr,
      T("Discard annotations and take a new screenshot?")) != 1) return;
  active_mode_ = shortcut ? 0 : unsigned(mode_->value());
  const int seconds[] = {0, 3, 5, 10};
  capture_at_ = monotonic() + (shortcut ? 0 : seconds[delay_->value()]);
  waiting_ = true; refresh(); tick();
}
void CaptureWindow::request(bool shortcut) {
  printf("[reliefcapture.elf] request shortcut=%d overlay=%d waiting=%d pending=%d job=%u dialog=%u modal=%d grab=%d\n",
         shortcut, overlay_ != nullptr, waiting_, capture_pending_, unsigned(job_.kind()), dialog_depth_, Fl::modal() != nullptr, Fl::grab() != nullptr); fflush(stdout);
  if (overlay_ || waiting_ || capture_pending_ || job_.kind() == Job::Kind::Capture) return;
  if (dialog_depth_ || Fl::modal() || Fl::grab() || job_.kind() == Job::Kind::Save) {
    if (shortcut) {
      // A fresh capture request supersedes an earlier close-after-save request.
      closing_ = false;
      queued_capture_ = true;
      message(T("Screenshot queued; finish the current dialog first"));
      puts("[reliefcapture.elf] capture-queued"); fflush(stdout);
    }
    return;
  }
  if (shortcut) begin_capture(true); else { show(); take_focus(); }
}
void CaptureWindow::cancel_capture() {
  printf("[reliefcapture.elf] capture-canceled stage=%s\n", job_.kind() == Job::Kind::Capture ? "reading" : "selection-or-delay"); fflush(stdout);
  waiting_ = capture_pending_ = false;
  release_cancel_key();
  if (job_.kind() == Job::Kind::Capture) job_.cancel();
  message(T("Capture canceled")); refresh();
  if (only_select_ && !document_.image().valid()) closed_ = true;
  else { show(); take_focus(); }
}
void CaptureWindow::capture_failed(const char *error) {
  Text message_text; message_text.set(error);
  cancel_capture(); closed_ = false; only_select_ = false;
  show(); take_focus(); message(T(message_text.c_str())); refresh();
}
void CaptureWindow::complete_capture(Image &&image) {
  document_.replace(static_cast<Image &&>(image)); dirty_ = true; path_[0] = 0;
  show(); take_focus(); layout(); refresh(); Fl::flush();
  printf("[reliefcapture.elf] captured size=%dx%d\n", document_.image().width, document_.image().height); fflush(stdout);
  copy_image();
}
void CaptureWindow::finish_overlay(Rect rectangle, bool canceled) {
  if (!overlay_) return;
  Image image; bool ok = !canceled && overlay_->image().crop(rectangle, image);
  overlay_->dismiss(); Fl::delete_widget(overlay_); overlay_ = nullptr;
  if (ok) complete_capture(static_cast<Image &&>(image));
  else if (canceled) cancel_capture();
  else capture_failed("Could not crop the screenshot");
}
void CaptureWindow::copy_image() {
  if (!document_.image().valid() || job_.busy()) return;
  Image rendered; Text error;
  message(T("Preparing screenshot...")); Fl::flush();
  if (!render_document(document_, rendered, error)) { message(T(error.c_str())); return; }
  Array<unsigned char> rgb;
  if (!rgb.resize(rendered.pixels.size() * 3)) { message(T("Not enough memory")); return; }
  for (size_t n = 0; n < rendered.pixels.size(); ++n) {
    rgb[n * 3] = rendered.pixels[n] >> 16; rgb[n * 3 + 1] = rendered.pixels[n] >> 8; rgb[n * 3 + 2] = rendered.pixels[n];
  }
  Fl::copy_image(rgb.data(), rendered.width, rendered.height, 1);
  message(T("Image copied; keep ReliefCapture open while pasting"));
  printf("[reliefcapture.elf] copied size=%dx%d\n", rendered.width, rendered.height); fflush(stdout);
}
bool CaptureWindow::save_image() {
  if (!document_.image().valid() || job_.busy() || waiting_ || capture_pending_) return false;
  DialogGuard dialog(dialog_depth_);
  char initial[PATH_MAX];
  if (path_[0]) snprintf(initial, sizeof(initial), "%s", path_);
  else {
    time_t current = time(nullptr); tm date{}; localtime_r(&current, &date); char name[64];
    strftime(name, sizeof(name), "Screenshot-%Y%m%d-%H%M%S.png", &date);
    const char *home = getenv("HOME"); snprintf(initial, sizeof(initial), "%s/%s", home && *home ? home : ".", name);
  }
  const char *selected = fl_file_chooser(T("Save screenshot"), "*.png", initial, 0);
  if (!selected) return false;
  char path[PATH_MAX];
  size_t n = strlen(selected); bool suffix = n >= 4 && !strcasecmp(selected + n - 4, ".png");
  if (n + (suffix ? 1 : 5) > sizeof(path)) { message(T("The file path is too long")); return false; }
  snprintf(path, sizeof(path), "%s%s", selected, suffix ? "" : ".png");
  Destination destination; Text error;
  if (!probe_destination(path, destination, error)) { message(T(error.c_str())); return false; }
  if (destination.exists && fl_choice("%s", T("Cancel"), T("Replace"), nullptr, T("Replace the existing screenshot?")) != 1) return false;
  Image rendered; message(T("Preparing screenshot...")); Fl::flush();
  if (!render_document(document_, rendered, error) || !job_.save(path, rendered, destination, error)) {
    message(T(error.empty() ? "Could not render the screenshot" : error.c_str())); return false;
  }
  snprintf(saving_path_, sizeof(saving_path_), "%s", path);
  message(T("Saving screenshot...")); refresh(); return true;
}
void CaptureWindow::undo(bool redo) {
  if (job_.busy() || waiting_ || capture_pending_) return;
  if (redo ? document_.redo() : document_.undo()) {
    dirty_ = true; refresh();
    printf("[reliefcapture.elf] history count=%zu\n", document_.visible()); fflush(stdout);
  }
}
void CaptureWindow::close_window() {
  DialogGuard dialog(dialog_depth_);
  if (job_.kind() == Job::Kind::Save) { closing_ = true; message(T("Finishing the screenshot save before closing...")); return; }
  if (dirty_) {
    int choice = fl_choice("%s", T("Cancel"), T("Save"), T("Discard"), T("Save this screenshot before closing?"));
    if (!choice) return;
    if (choice == 1) { if (save_image()) closing_ = true; return; }
  }
  job_.cancel(); waiting_ = capture_pending_ = false; hide(); closed_ = true;
}
void CaptureWindow::help() {
  DialogGuard dialog(dialog_depth_);
  fl_message_title("ReliefCapture");
  fl_message("%s", T("Win+Shift+S: select a screenshot\nCtrl+N: new capture\nCtrl+C: copy image\nCtrl+S: save PNG\nCtrl+Z: undo\nCtrl+Y / Ctrl+Shift+Z: redo\n\nSelection: R region, W window, F full screen, Esc cancel.\nWindow captures include visible decorations and occlusions.\nImages remain in the clipboard while ReliefCapture is running."));
}
void CaptureWindow::timer(void *p) { auto *s = static_cast<CaptureWindow *>(p); s->tick(); if (!s->closed_) Fl::repeat_timeout(.025, timer, s); }
void CaptureWindow::tick() {
  double now = monotonic();
  if (queued_capture_ && !dialog_depth_ && !Fl::modal() && !Fl::grab() &&
      !job_.busy() && !overlay_ && !waiting_ && !capture_pending_ && !closing_) {
    queued_capture_ = false; begin_capture(true); return;
  }
  if (waiting_) {
    if (now >= capture_at_) {
      // Reserve Escape before hiding: queued keys may still target the old XID.
      if (!grab_cancel_key()) { capture_failed("Could not reserve Escape for capture cancellation"); return; }
      waiting_ = false; capture_pending_ = true; hide(); Fl::flush(); hidden_at_ = now + .25;
    }
    else { char msg[160]; snprintf(msg, sizeof(msg), T("Capture in %d seconds; click Cancel to stop"), int(ceil(capture_at_ - now))); message(msg); }
  }
  if (capture_pending_ && now >= hidden_at_) {
    capture_pending_ = false; Text error;
    job_.x_fd(fl_display ? ConnectionNumber(fl_display) : -1);
    if (!job_.capture(error)) capture_failed(error.c_str());
    else { puts("[reliefcapture.elf] capture-reading"); fflush(stdout); }
  }
  auto kind = job_.kind();
  if (job_.poll()) {
    if (kind == Job::Kind::Capture) release_cancel_key();
    if (!job_.ok()) {
      Text error = job_.error(); closing_ = false;
      if (kind == Job::Kind::Capture) capture_failed(error.c_str());
      else { message(T(error.c_str())); show(); refresh(); }
      return;
    }
    if (kind == Job::Kind::Capture) {
      Snapshot snapshot = job_.take();
      if (active_mode_ == 2) complete_capture(static_cast<Image &&>(snapshot.image));
      else {
        Fl_Group *previous = Fl_Group::current(); Fl_Group::current(nullptr);
        overlay_ = new Overlay(static_cast<Snapshot &&>(snapshot), active_mode_ == 1,
          [](Rect r, bool canceled, void *p) { static_cast<CaptureWindow *>(p)->finish_overlay(r, canceled); }, this);
        Fl_Group::current(previous); overlay_->present();
      }
    } else {
      snprintf(path_, sizeof(path_), "%s", saving_path_); dirty_ = false;
      message(T("Screenshot saved")); printf("[reliefcapture.elf] saved PNG\n"); fflush(stdout); refresh();
      if (closing_) { hide(); closed_ = true; }
    }
  }
}
bool CaptureWindow::hotkey() {
  int key = Fl::event_key();
  if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
  if (key == FL_Escape && (waiting_ || capture_pending_ || job_.kind() == Job::Kind::Capture)) { cancel_capture(); return true; }
  if (!(Fl::event_state() & FL_CTRL)) return false;
  if (key == 'n') begin_capture(false);
  else if (key == 'c') copy_image();
  else if (key == 's') save_image();
  else if (key == 'z') undo((Fl::event_state() & FL_SHIFT) != 0);
  else if (key == 'y') undo(true);
  else return false;
  return true;
}
int CaptureWindow::handle(int event) {
  if ((event == FL_KEYDOWN || event == FL_SHORTCUT) && !dialog_depth_ && !Fl::grab() && hotkey()) return 1;
  return Fl_Double_Window::handle(event);
}
bool CaptureWindow::grab_cancel_key() {
  return escape_key_.acquire(fl_display);
}
void CaptureWindow::release_cancel_key() {
  escape_key_.release();
}
int CaptureWindow::system_event(void *raw, void *data) {
  auto *self = static_cast<CaptureWindow *>(data); auto *event = static_cast<XEvent *>(raw);
  if (!self->escape_key_.matches(*event)) return 0;
  if (event->type == KeyPress) self->cancel_capture();
  return 1;
}
}
