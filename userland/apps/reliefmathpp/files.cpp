#include "window.h"
#include <math.h>
#include <stdio.h>
namespace rmp {
void MathWindow::file_action(Command command) {
  if (io_.busy() || !apply_fields())
    return;
  Text err;
  if (command == Command::Open) {
    const char *p =
        fl_file_chooser("打开 ReliefMath++ 文档", "*.rmathpp",
                        path_.empty() ? getenv("HOME") : path_.c_str(), 0);
    if (!p)
      return;
    if (!pending_path_.set(p) || !io_.read(p, err)) {
      fl_alert("%s", err.empty() ? "内存不足" : err.c_str());
      return;
    }
    operation_ = Operation::Open;
    stage_ = 1;
  } else {
    if (!encode(document, pending_bytes_, err)) {
      fl_alert("%s", err.c_str());
      return;
    }
    if (command == Command::Save && !path_.empty()) {
      pending_path_ = path_;
      operation_ = Operation::Save;
      stage_ = 1;
      if (!io_.write(path_.c_str(), pending_bytes_, stamp_, err)) {
        operation_ = Operation::Idle;
        fl_alert("%s", err.c_str());
        return;
      }
    } else {
      const char *p = fl_file_chooser(
          "保存 ReliefMath++ 文档", "*.rmathpp",
          path_.empty() ? "Untitled.rmathpp" : path_.c_str(), 0);
      if (!p)
        return;
      start_write(p, false);
      return;
    }
  }
  top_->deactivate();
  quick_->deactivate();
  tools_->deactivate();
  body_->deactivate();
  update_status();
}
void MathWindow::load_initial(const char *path) {
  Text err;
  if (!pending_path_.set(path) || !io_.read(path, err)) {
    fl_alert("%s", err.c_str());
    return;
  }
  operation_ = Operation::Open;
  stage_ = 1;
  top_->deactivate();
  quick_->deactivate();
  tools_->deactivate();
  body_->deactivate();
  update_status();
}
void MathWindow::start_write(const char *path, bool export_file) {
  Text err;
  if (!pending_path_.set(path) ||
      !io_.probe(path, err, export_file ? path_.c_str() : nullptr)) {
    fl_alert("%s", err.empty() ? "内存不足" : err.c_str());
    return;
  }
  operation_ = export_file ? Operation::Export : Operation::Save;
  stage_ = 0;
  top_->deactivate();
  quick_->deactivate();
  tools_->deactivate();
  body_->deactivate();
  update_status();
}
void MathWindow::poll_io() {
  if (!io_.busy() || !io_.poll())
    return;
  rt::IoJob::Reply reply = io_.take();
  Text err;
  if (reply.ok && stage_ == 0) {
    if (!reply.stamp.exists ||
        fl_choice("目标文件已存在，是否覆盖？", "取消", "覆盖", nullptr) == 1) {
      stage_ = 1;
      if (io_.write(pending_path_.c_str(), pending_bytes_, reply.stamp, err,
                    operation_ == Operation::Export ? path_.c_str()
                                                    : nullptr)) {
        update_status();
        return;
      }
      reply.ok = false;
      reply.error = err;
    } else {
      operation_ = Operation::Idle;
      closing_ = false;
    }
  } else if (reply.ok && operation_ == Operation::Open) {
    Document next;
    if (!decode(reply.bytes.data(), reply.bytes.size(), next, err)) {
      reply.ok = false;
      reply.error = err;
    } else {
      document = static_cast<Document &&>(next);
      path_ = pending_path_;
      stamp_ = reply.stamp;
      history_.clear();
      dirty_ = fields_dirty_ = false;
      active_ = document.objects.empty() ? 0 : document.objects[0].id;
      pick_kind_ = Kind::Count;
      refill();
      refresh();
      select_tab(document.view.tab);
      double values[] = {document.vars.a, document.vars.b, document.vars.c,
                         document.vars.d};
      for (int n = 0; n < 4; ++n) {
        char b[40];
        snprintf(b, sizeof(b), "%.10g", values[n]);
        parameter_[n]->value(b);
        sliders_[n]->value(fmax(-5, fmin(5, values[n])));
      }
      resample();
      printf("[reliefmathpp.elf] opened objects=%zu\n",
             document.objects.size());
      fflush(stdout);
    }
  } else if (reply.ok && operation_ == Operation::Save) {
    path_ = pending_path_;
    stamp_ = reply.stamp;
    dirty_ = false;
    printf("[reliefmathpp.elf] saved objects=%zu\n", document.objects.size());
    fflush(stdout);
    if (closing_)
      hide();
  } else if (reply.ok && operation_ == Operation::Export) {
    puts("[reliefmathpp.elf] export complete");
    fflush(stdout);
  }
  top_->activate();
  quick_->activate();
  tools_->activate();
  body_->activate();
  if (!current())
    properties_->deactivate();
  if (!reply.ok) {
    fl_alert("%s", reply.error.c_str());
    closing_ = false;
  }
  operation_ = Operation::Idle;
  pending_bytes_.clear();
  update_status();
}
void MathWindow::export_action(Command command) {
  if (sampling_) {
    fl_message("采样尚未完成，请稍后导出。");
    return;
  }
  Text err;
  Bytes bytes;
  bool ok = command == Command::ExportSvg
                ? canvas[tab()]->export_svg(bytes, err)
                : canvas[tab()]->export_png(bytes, err);
  if (!ok) {
    fl_alert("%s", err.empty() ? "不能生成图片" : err.c_str());
    return;
  }
  const char *path = fl_file_chooser(
      "导出当前视图", command == Command::ExportSvg ? "*.svg" : "*.png",
      command == Command::ExportSvg ? "plot.svg" : "plot.png", 0);
  if (!path)
    return;
  pending_bytes_.swap(bytes);
  start_write(path, true);
}
} // namespace rmp
