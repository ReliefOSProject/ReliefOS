#include "window.h"
#include <FL/Fl_File_Chooser.H>
#include <stdio.h>
#include <string.h>
namespace rp {
namespace {
const char *state_label(State state) {
  switch (state) {
  case State::Idle:
    return T("Open an MP3 or WAV file to begin");
  case State::Loading:
    return T("Reading audio information...");
  case State::Playing:
    return T("Playing audio");
  case State::Paused:
    return T("Paused");
  case State::Stopped:
    return T("Stopped");
  case State::Ended:
    return T("Playback complete");
  case State::Failed:
    return T("Audio playback failed");
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
  open_ = new Fl_Button(0, 0, 1, 1, T("Open audio..."));
  help_ = new Fl_Button(0, 0, 1, 1, T("Help"));
  panel_ = new Fl_Box(0, 0, 1, 1);
  panel_->box(FL_ENGRAVED_BOX);
  title_ = new Fl_Box(0, 0, 1, 1, T("No audio file selected"));
  title_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  title_->labelfont(FL_HELVETICA_BOLD);
  title_->labelsize(19);
  detail_ = new Fl_Box(0, 0, 1, 1, "MP3 / WAV");
  detail_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
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
  size_range(600, 330);
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
  const char *path =
      fl_file_chooser(T("Open audio"), T("Audio files (*.{mp3,MP3,wav,WAV})"),
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
    status_->copy_label(T("Stopping audio..."));
}
void PlayerWindow::update() {
  const auto &s = session_.status();
  char text[256], elapsed[40], total[40];
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
  if (s.info.rate)
    snprintf(text, sizeof(text), "%s  ·  %u Hz  ·  %s  ·  %u %s",
             s.info.kind == Kind::Mp3 ? "MP3" : "WAV", s.info.rate,
             s.info.channels == 1 ? T("Mono audio") : T("Stereo audio"), s.info.bits,
             T("bit"));
  else
    snprintf(text, sizeof(text), "MP3 / WAV");
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
        T("ReliefPlayer\nSimple MP3 and WAV audio player\n\nCtrl+O: open "
          "audio\nSpace: play or pause\nLeft / Right: seek 10 seconds\nHome: "
          "return to the beginning\n\nVolume changes affect this player "
          "only.\nSupports PCM WAV 8/16/24/32-bit and float32, mono or "
          "stereo.\nLocal files only; no network streams or media library."));
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
