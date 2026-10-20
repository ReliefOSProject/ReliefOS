#include "canvas.h"
#include "window.h"
#include <stdio.h>
namespace rd {
namespace {
bool filename(Text &out, const char *path, const char *suffix) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (strchr(base, '.'))
    return out.set(path);
  size_t n = strlen(path), extra = strlen(suffix);
  if (n + extra >= 4096)
    return false;
  char buf[4096];
  memcpy(buf, path, n);
  memcpy(buf + n, suffix, extra + 1);
  return out.set(buf);
}
} // namespace
void DiagramWindow::busy_ui(bool active) {
  Fl_Group *groups[] = {menus_, quick_, tools_, body_};
  for (Fl_Group *g : groups) {
    if (active)
      g->deactivate();
    else
      g->activate();
  }
  if (!active)
    refresh();
  update_status();
}
void DiagramWindow::file_action(Command c) {
  if (busy() || !apply_fields())
    return;
  Text e;
  if (c == Command::Open) {
    const char *path =
        fl_file_chooser("打开 ReliefDiagram 文档", "*.rdiagram",
                        path_.empty() ? getenv("HOME") : path_.c_str(), 0);
    if (!path)
      return;
    load_initial(path);
    return;
  }
  if (!encode(document, pending_bytes_, e)) {
    fl_alert("%s", e.c_str());
    return;
  }
  if (c == Command::Save && !path_.empty()) {
    pending_path_ = path_;
    if (!io_.write(path_.c_str(), pending_bytes_, stamp_, e)) {
      fl_alert("%s", e.c_str());
      return;
    }
    operation_ = Operation::Save;
    stage_ = 1;
    busy_ui(true);
  } else {
    const char *path =
        fl_file_chooser("保存 ReliefDiagram 文档", "*.rdiagram",
                        path_.empty() ? "Untitled.rdiagram" : path_.c_str(), 0);
    if (!path)
      return;
    Text target;
    if (!filename(target, path, ".rdiagram")) {
      fl_alert("文件名过长或内存不足");
      return;
    }
    start_write(target.c_str(), false);
  }
}
void DiagramWindow::load_initial(const char *path) {
  if (busy())
    return;
  Text e;
  if (!pending_path_.set(path) || !io_.read(path, e)) {
    fl_alert("%s", e.empty() ? "内存不足" : e.c_str());
    return;
  }
  operation_ = Operation::Open;
  stage_ = 1;
  busy_ui(true);
}
void DiagramWindow::start_write(const char *path, bool exporting) {
  Text e;
  if (!pending_path_.set(path) ||
      !io_.probe(pending_path_.c_str(), e,
                 exporting ? path_.c_str() : nullptr)) {
    operation_ = Operation::Idle;
    resume_pending_ = false;
    busy_ui(false);
    fl_alert("%s", e.empty() ? "内存不足" : e.c_str());
    return;
  }
  operation_ = exporting ? Operation::Export : Operation::Save;
  stage_ = 0;
  busy_ui(true);
}
void DiagramWindow::poll_io() {
  if (!io_.busy() || !io_.poll())
    return;
  rt::IoJob::Reply reply = io_.take();
  Text e;
  bool resume = false;
  Command action = resume_;
  if (reply.ok && stage_ == 0) {
    if (reply.stamp.exists &&
        fl_choice("目标文件已存在，是否覆盖？", "取消", "覆盖", nullptr) != 1) {
      operation_ = Operation::Idle;
      resume_pending_ = false;
      pending_bytes_.clear();
      busy_ui(false);
      return;
    }
    stage_ = 1;
    if (io_.write(pending_path_.c_str(), pending_bytes_, reply.stamp, e,
                  operation_ == Operation::Export ? path_.c_str() : nullptr)) {
      update_status();
      return;
    }
    reply.ok = false;
    reply.error = e;
  } else if (reply.ok && operation_ == Operation::Open) {
    Document next;
    if (!decode(reply.bytes.data(), reply.bytes.size(), next, e)) {
      reply.ok = false;
      reply.error = e;
    } else {
      document = static_cast<Document &&>(next);
      path_ = pending_path_;
      stamp_ = reply.stamp;
      selection.clear();
      history_.clear();
      dirty_ = fields_dirty_ = false;
      notice_.clear();
      changed();
      printf("[reliefdiagram.elf] opened nodes=%zu edges=%zu\n",
             document.nodes.size(), document.edges.size());
      fflush(stdout);
    }
  } else if (reply.ok && operation_ == Operation::Save) {
    path_ = pending_path_;
    stamp_ = reply.stamp;
    dirty_ = false;
    update_status("文档已保存");
    printf("[reliefdiagram.elf] saved nodes=%zu edges=%zu\n",
           document.nodes.size(), document.edges.size());
    fflush(stdout);
    resume = resume_pending_;
    resume_pending_ = false;
  } else if (reply.ok && operation_ == Operation::Export) {
    update_status("导出完成");
    puts("[reliefdiagram.elf] export complete");
    fflush(stdout);
  }
  if (!reply.ok) {
    resume_pending_ = false;
    fl_alert("%s", reply.error.c_str());
  }
  operation_ = Operation::Idle;
  pending_bytes_.clear();
  busy_ui(false);
  if (resume)
    run(action);
}
void DiagramWindow::export_action(Command c) {
  if (busy() || !apply_fields() || !canvas->prepare())
    return;
  const char *suffix = c == Command::ExportSvg   ? ".svg"
                       : c == Command::ExportPng ? ".png"
                                                 : ".md";
  const char *filter = c == Command::ExportSvg   ? "*.svg"
                       : c == Command::ExportPng ? "*.png"
                                                 : "*.md";
  const char *default_name = c == Command::ExportSvg   ? "diagram.svg"
                             : c == Command::ExportPng ? "diagram.png"
                                                       : "diagram.md";
  const char *path = fl_file_chooser(
      c == Command::ExportOutline ? "导出 Markdown 大纲" : "导出完整图形",
      filter, default_name, 0);
  if (!path)
    return;
  Text target, e;
  if (!filename(target, path, suffix)) {
    fl_alert("文件名过长或内存不足");
    return;
  }
  if (c == Command::ExportPng) {
    if (!pending_path_.set(target.c_str()) || !png_.start(canvas->frame(), e)) {
      fl_alert("%s", e.empty() ? "内存不足" : e.c_str());
      return;
    }
    operation_ = Operation::Png;
    busy_ui(true);
    return;
  }
  bool ok = c == Command::ExportSvg ? svg(canvas->frame(), pending_bytes_, e)
                                    : markdown(document, pending_bytes_, e);
  if (!ok) {
    fl_alert("%s", e.c_str());
    return;
  }
  start_write(target.c_str(), true);
}
} // namespace rd
