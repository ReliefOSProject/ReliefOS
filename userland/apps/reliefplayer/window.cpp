#include "window.h"
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_draw.H>
#include <stdio.h>
#include <string.h>
namespace rp {
namespace {
class VideoView : public Fl_Widget {
  const Session &session_;
  int drawn_width_ = 1, drawn_height_ = 1;
  static void row(void *opaque, int x, int y, int width, unsigned char *out) {
    auto &s = *static_cast<VideoView *>(opaque);
    unsigned source_y =
        unsigned(y) * s.session_.height() / unsigned(s.drawn_height_);
    const unsigned char *line =
        s.session_.pixels() + source_y * s.session_.width() * 3;
    for (int n = 0; n < width; ++n) {
      unsigned source_x =
          unsigned(x + n) * s.session_.width() / unsigned(s.drawn_width_);
      memcpy(out + n * 3, line + source_x * 3, 3);
    }
  }
  void draw() override {
    fl_push_clip(x(), y(), w(), h());
    fl_color(FL_BLACK);
    fl_rectf(x(), y(), w(), h());
    if (session_.pixels() && session_.width() && session_.height()) {
      drawn_width_ = w();
      drawn_height_ = int(int64_t(w()) * session_.height() / session_.width());
      if (drawn_height_ > h()) {
        drawn_height_ = h();
        drawn_width_ = int(int64_t(h()) * session_.width() / session_.height());
      }
      if (drawn_width_ > 0 && drawn_height_ > 0)
        fl_draw_image(row, this, x() + (w() - drawn_width_) / 2,
                      y() + (h() - drawn_height_) / 2, drawn_width_,
                      drawn_height_, 3);
    }
    fl_pop_clip();
  }

public:
  explicit VideoView(const Session &session)
      : Fl_Widget(0, 0, 1, 1), session_(session) {}
};
const char *state_label(State state) {
  switch (state) {
  case State::Idle:
    return T("Open an MP3, WAV or MP4 file to begin");
  case State::Loading:
    return T("Reading media information...");
  case State::Playing:
    return T("Playing");
  case State::Paused:
    return T("Paused");
  case State::Stopped:
    return T("Stopped");
  case State::Ended:
    return T("Playback complete");
  case State::Failed:
    return T("Media playback failed");
  }
  return "";
}
void clock_text(char *text, size_t capacity, uint64_t samples, unsigned rate) {
  uint64_t seconds = rate ? samples / rate : 0;
  if (seconds >= 3600)
    snprintf(text, capacity, "%llu:%02u:%02u",
             (unsigned long long)(seconds / 3600), unsigned(seconds / 60 % 60),
             unsigned(seconds % 60));
  else
    snprintf(text, capacity, "%02u:%02u", unsigned(seconds / 60),
             unsigned(seconds % 60));
}
} // namespace
int ProgressSlider::handle(int event) {
  if (event == FL_PUSH)
    dragging = true;
  int result = Fl_Slider::handle(event);
  if (event == FL_RELEASE || event == FL_UNFOCUS)
    dragging = false;
  return result;
}
PlayerWindow::PlayerWindow() : Fl_Double_Window(720, 350, "ReliefPlayer") {
  xclass("reliefos-reliefplayer");
  begin();
  open_ = new Fl_Button(0, 0, 1, 1, T("Open media..."));
  help_ = new Fl_Button(0, 0, 1, 1, T("Help"));
  panel_ = new Fl_Box(0, 0, 1, 1);
  panel_->box(FL_ENGRAVED_BOX);
  title_ = new Fl_Box(0, 0, 1, 1, T("No media file selected"));
  title_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  title_->labelfont(FL_HELVETICA_BOLD);
  title_->labelsize(19);
  detail_ = new Fl_Box(0, 0, 1, 1, "MP3 / WAV / MP4");
  detail_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  video_ = new VideoView(session_);
  video_->hide();
  elapsed_ = new Fl_Box(0, 0, 1, 1, "00:00");
  elapsed_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  duration_ = new Fl_Box(0, 0, 1, 1, "00:00");
  duration_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  seek_ = new ProgressSlider;
  seek_->type(FL_HOR_NICE_SLIDER);
  seek_->bounds(0, 1);
  seek_->step(.001);
  seek_->when(FL_WHEN_RELEASE | FL_WHEN_NOT_CHANGED);
  seek_->tooltip(T("Drag to seek; Left/Right: 10 seconds"));
  play_ = new Fl_Button(0, 0, 1, 1, T("Play"));
  play_->tooltip(T("Play or pause (Space)"));
  stop_ = new Fl_Button(0, 0, 1, 1, T("Stop"));
  back_ = new Fl_Button(0, 0, 1, 1, T("Back 10 s"));
  forward_ = new Fl_Button(0, 0, 1, 1, T("Forward 10 s"));
  volume_ = new Fl_Value_Slider(0, 0, 1, 1, T("Volume"));
  volume_->align(FL_ALIGN_LEFT);
  volume_->type(FL_HOR_NICE_SLIDER);
  volume_->bounds(0, 100);
  volume_->step(1);
  volume_->value(70);
  volume_->textsize(14);
  mute_ = new Fl_Check_Button(0, 0, 1, 1, T("Mute"));
  status_ = new Fl_Box(0, 0, 1, 1);
  status_->box(FL_DOWN_BOX);
  status_->labelsize(13);
  status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  Fl_Widget *controls[] = {open_, help_,    seek_,   play_, stop_,
                           back_, forward_, volume_, mute_};
  for (auto *control : controls)
    control->callback(action, this);
  callback(action, this);
  end();
  resizable(this);
  size_range(600, 350);
  layout();
  update();
  Fl::add_timeout(.03, tick, this);
}
PlayerWindow::~PlayerWindow() { Fl::remove_timeout(tick, this); }
void PlayerWindow::layout() {
  open_->resize(16, 14, 124, 32);
  help_->resize(w() - 96, 14, 80, 32);
  panel_->resize(16, 62, w() - 32, 84);
  title_->resize(28, 73, w() - 56, 29);
  detail_->resize(28, 108, w() - 56, 24);
  int y = h() - 190;
  video_->resize(16, 156, w() - 32, y > 166 ? y - 166 : 1);
  elapsed_->resize(16, y, 120, 24);
  duration_->resize(w() - 136, y, 120, 24);
  seek_->resize(16, y + 27, w() - 32, 25);
  play_->resize(16, y + 65, 108, 36);
  stop_->resize(136, y + 65, 92, 36);
  back_->resize(240, y + 65, 104, 36);
  forward_->resize(356, y + 65, 104, 36);
  volume_->resize(72, y + 120, w() - 196, 27);
  mute_->resize(w() - 108, y + 119, 92, 28);
  status_->resize(0, h() - 28, w(), 28);
}
void PlayerWindow::resize(int x, int y, int width, int height) {
  Fl_Double_Window::resize(x, y, width, height);
  layout();
}
void PlayerWindow::open_path(const char *path) {
  if (!path || !*path)
    return;
  if (strlen(path) >= sizeof(path_)) {
    fl_alert("%s", T("The file path is too long"));
    return;
  }
  if (!session_.open(path, mute_->value() ? 0 : unsigned(volume_->value())))
    return;
  video_->hide();
  video_serial_ = 0;
  video_layout_ = false;
  size_range(600, 350);
  if (path != path_)
    memcpy(path_, path, strlen(path) + 1);
  const char *base = strrchr(path_, '/');
  title_->copy_label(base ? base + 1 : path_);
  title_->copy_tooltip(path_);
  char title[300];
  snprintf(title, sizeof(title), "ReliefPlayer — %.240s",
           base ? base + 1 : path);
  copy_label(title);
  update();
}
void PlayerWindow::choose() {
  const char *path = fl_file_chooser(
      T("Open media"), T("Media files (*.{mp3,MP3,wav,WAV,mp4,MP4})"),
      path_[0] ? path_ : nullptr, 0);
  if (path)
    open_path(path);
}
void PlayerWindow::toggle() {
  const auto &s = session_.status();
  if (s.state == State::Loading)
    return;
  if (!path_[0]) {
    choose();
    return;
  }
  if (!session_.active()) {
    open_path(path_);
    return;
  }
  session_.command(Op::Play, s.state == State::Playing ? 0 : 1);
}
void PlayerWindow::skip(int seconds) {
  const auto &s = session_.status();
  if (!s.info.rate)
    return;
  int64_t position = int64_t(s.position) + int64_t(seconds) * s.info.rate;
  if (position < 0)
    position = 0;
  if (uint64_t(position) > s.info.frames)
    position = int64_t(s.info.frames);
  session_.command(Op::Seek, uint64_t(position));
}
void PlayerWindow::close_player() {
  if (closing_)
    return;
  closing_ = true;
  session_.shutdown();
  open_->deactivate();
  help_->deactivate();
  if (!session_.active())
    hide();
  else
    status_->copy_label(T("Stopping playback..."));
}
void PlayerWindow::update() {
  const auto &s = session_.status();
  char text[256], elapsed[40], total[40];
  if (s.info.width && s.state != State::Loading) {
    if (!video_layout_) {
      video_layout_ = true;
      size_range(600, 500);
      if (h() < 600) {
        int sx, sy, sw, sh;
        Fl::screen_work_area(sx, sy, sw, sh, x(), y());
        int width = w() < 800 ? 800 : w(), height = 640;
        if (width > sw - 32)
          width = sw - 32;
        if (height > sh - 60)
          height = sh - 60;
        if (width < 600)
          width = 600;
        if (height < 500)
          height = 500;
        int left = x(), top = y();
        if (left + width > sx + sw)
          left = sx + sw - width;
        if (top + height > sy + sh - 30)
          top = sy + sh - height - 30;
        if (left < sx)
          left = sx;
        if (top < sy + 30)
          top = sy + 30;
        resize(left, top, width, height);
      }
      video_->show();
      layout();
      redraw();
    }
    if (video_serial_ != session_.video_serial()) {
      video_serial_ = session_.video_serial();
      video_->redraw();
    }
  } else if (video_layout_) {
    video_layout_ = false;
    video_->hide();
    size_range(600, 350);
    redraw();
  }
  play_->copy_label(s.state == State::Playing ? T("Pause") : T("Play"));
  bool loaded = s.info.frames && s.state != State::Loading &&
                session_.active() && !closing_;
  if (loaded) {
    stop_->activate();
    back_->activate();
    forward_->activate();
    seek_->activate();
  } else {
    stop_->deactivate();
    back_->deactivate();
    forward_->deactivate();
    seek_->deactivate();
  }
  if (s.state == State::Loading || closing_)
    play_->deactivate();
  else
    play_->activate();
  clock_text(elapsed, sizeof(elapsed), s.position, s.info.rate);
  elapsed_->copy_label(elapsed);
  clock_text(total, sizeof(total), s.info.frames, s.info.rate);
  duration_->copy_label(total);
  if (!seek_->dragging)
    seek_->value(s.info.frames ? double(s.position) / double(s.info.frames)
                               : 0);
  if (s.info.width)
    snprintf(text, sizeof(text), "MP4  ·  %u × %u  ·  %s", s.info.width,
             s.info.height,
             s.info.has_audio ? T("Stereo audio") : T("No audio track"));
  else if (s.info.rate)
    snprintf(text, sizeof(text), "%s  ·  %u Hz  ·  %s  ·  %u %s",
             s.info.kind == Kind::Mp3 ? "MP3" : "WAV", s.info.rate,
             s.info.channels == 1 ? T("Mono audio") : T("Stereo audio"),
             s.info.bits, T("bit"));
  else
    snprintf(text, sizeof(text), "MP3 / WAV / MP4");
  detail_->copy_label(text);
  if (closing_)
    return;
  if (s.state == State::Failed)
    snprintf(text, sizeof(text), "%s%s%d", T(error_text(s.error)),
             s.system_error ? " · errno " : "", s.system_error);
  else if (s.state == State::Loading)
    snprintf(text, sizeof(text), "%s %u%%", state_label(s.state), s.progress);
  else
    snprintf(text, sizeof(text), "%s", state_label(s.state));
  if (s.state == State::Failed && !s.system_error)
    snprintf(text, sizeof(text), "%s", T(error_text(s.error)));
  status_->copy_label(text);
  status_->copy_tooltip(text);
}
void PlayerWindow::tick(void *p) {
  auto &window = *static_cast<PlayerWindow *>(p);
  window.session_.poll();
  window.update();
  if (window.closing_ && !window.session_.active()) {
    window.hide();
    return;
  }
  Fl::repeat_timeout(.03, tick, p);
}
void PlayerWindow::action(Fl_Widget *widget, void *p) {
  auto &window = *static_cast<PlayerWindow *>(p);
  if (widget == &window)
    window.close_player();
  else if (widget == window.open_)
    window.choose();
  else if (widget == window.play_)
    window.toggle();
  else if (widget == window.stop_)
    window.session_.command(Op::Stop);
  else if (widget == window.back_)
    window.skip(-10);
  else if (widget == window.forward_)
    window.skip(10);
  else if (widget == window.seek_)
    window.session_.command(
        Op::Seek, uint64_t(window.seek_->value() *
                           double(window.session_.status().info.frames)));
  else if (widget == window.volume_ || widget == window.mute_)
    window.session_.command(
        Op::Volume,
        window.mute_->value() ? 0 : unsigned(window.volume_->value()));
  else if (widget == window.help_)
    fl_message(
        "%s",
        T("ReliefPlayer\nMP3 / WAV audio and MP4 video player\n\nCtrl+O: open "
          "media\nSpace: play or pause\nLeft / Right: seek 10 seconds\nHome: "
          "return to the beginning\n\nVolume changes affect this player "
          "only.\nSupports PCM WAV 8/16/24/32-bit and float32, mono or "
          "stereo.\nMP4: H.264, MPEG-4 or HEVC video; AAC or MP3 audio.\n"
          "Local files only; no network streams or media library."));
}
bool PlayerWindow::hotkey() {
  if (!closing_ && !Fl::modal()) {
    int key = Fl::event_key();
    if (key == 'o' && Fl::event_state(FL_CTRL)) {
      choose();
      return 1;
    }
    if (key == ' ') {
      toggle();
      return 1;
    }
    if (key == FL_Left || key == FL_Right) {
      skip(key == FL_Left ? -10 : 10);
      return 1;
    }
    if (key == FL_Home) {
      session_.command(Op::Seek, 0);
      return 1;
    }
  }
  return false;
}
int PlayerWindow::handle(int event) {
  if (event == FL_SHORTCUT && hotkey())
    return 1;
  return Fl_Double_Window::handle(event);
}
} // namespace rp
