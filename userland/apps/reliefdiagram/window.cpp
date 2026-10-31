#include "window.h"
#include "canvas.h"
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Menu_Button.H>
#include <math.h>
#include <stdio.h>
namespace rd {
namespace {
Fl_Box *caption(int x, int y, int w, const char *s) {
  auto *b = new Fl_Box(x, y, w, 20, s);
  b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  b->labelsize(13);
  return b;
}
Fl_Button *button(int x, int y, int w, const char *s, Command c,
                  Fl_Callback *cb) {
  auto *b = new Fl_Button(x, y, w, 28, s);
  b->callback(cb, reinterpret_cast<void *>(intptr_t(c)));
  return b;
}
} // namespace
DiagramWindow::DiagramWindow() : Fl_Double_Window(1240, 780, "ReliefDiagram") {
  size_range(1100, 680);
  callback(
      [](Fl_Widget *, void *p) {
        static_cast<DiagramWindow *>(p)->run(Command::Close);
      },
      this);
  begin();
  menus_ = new Fl_Group(0, 0, w(), 32);
  const char *names[] = {T("File"), T("Edit"), T("View"), "图形", T("Help")};
  Fl_Menu_Button *menus[5];
  for (int n = 0; n < 5; ++n)
    menus[n] = new Fl_Menu_Button(8 + n * 76, 2, 70, 28, names[n]);
  auto item = [&](int n, const char *s, Command c, int key = 0) {
    menus[n]->add(s, key, click, reinterpret_cast<void *>(intptr_t(c)));
  };
  item(0, "新建空白图", Command::New, FL_CTRL + 'n');
  item(0, "打开…", Command::Open, FL_CTRL + 'o');
  item(0, "保存", Command::Save, FL_CTRL + 's');
  item(0, "另存为…", Command::SaveAs, FL_CTRL + FL_SHIFT + 's');
  item(0, "文档标题…", Command::Title);
  item(0, "导出 SVG…", Command::ExportSvg);
  item(0, "导出 PNG…", Command::ExportPng);
  item(0, "导出 Markdown 大纲…", Command::ExportOutline);
  item(0, "退出", Command::Close, FL_ALT + FL_F + 4);
  item(1, "撤销", Command::Undo, FL_CTRL + 'z');
  item(1, "重做", Command::Redo, FL_CTRL + 'y');
  item(1, "复制", Command::Copy, FL_CTRL + 'c');
  item(1, "粘贴", Command::Paste, FL_CTRL + 'v');
  item(1, "创建副本", Command::Duplicate, FL_CTRL + 'd');
  item(1, "删除（含子主题）", Command::Delete);
  item(1, "全选", Command::SelectAll, FL_CTRL + 'a');
  item(1, "应用属性", Command::Apply, FL_CTRL + FL_Enter);
  item(2, "适合内容", Command::Fit, FL_Home);
  item(2, "实际大小", Command::ActualSize);
  item(2, "放大", Command::ZoomIn);
  item(2, "缩小", Command::ZoomOut);
  item(2, "网格", Command::Grid);
  item(2, "吸附", Command::Snap);
  item(3, "添加子主题", Command::Child);
  item(3, "添加同级主题", Command::Sibling);
  item(3, "折叠或展开分支", Command::Fold);
  item(3, "平衡思维导图", Command::MindLayout);
  item(3, "向右树形布局", Command::TreeLayout);
  item(3, "流程分层布局", Command::FlowLayout);
  item(3, "置于顶层", Command::Front);
  item(3, "置于底层", Command::Back);
  item(4, "使用说明", Command::Help, FL_F + 1);
  title_ = caption(418, 5, w() - 430,
                   "ReliefDiagram   思维导图 · 流程图 · 逻辑关系");
  title_->labelsize(16);
  menus_->end();
  quick_ = new Fl_Group(0, 35, w(), 32);
  const char *quick_names[] = {"新建", "打开", "保存",     "撤销",    "重做",
                               "复制", "粘贴", "SVG 导出", "PNG 导出"};
  Command quick_commands[] = {
      Command::New,   Command::Open,      Command::Save,
      Command::Undo,  Command::Redo,      Command::Copy,
      Command::Paste, Command::ExportSvg, Command::ExportPng};
  int bx = 8;
  for (int n = 0; n < 9; ++n) {
    int width = n < 7 ? 64 : 92;
    button(bx, 36, width, quick_names[n], quick_commands[n], click);
    bx += width + 6;
  }
  quick_->end();
  build_tools();
  body_ = new Fl_Group(0, 180, w(), h() - 210);
  left_ = new Fl_Group(8, 181, 152, h() - 215);
  caption(10, 183, 148, "图形库 · 双击添加");
  shapes_ = new Fl_Hold_Browser(10, 207, 148, 190);
  shapes_->textsize(14);
  shapes_->format_char(0);
  for (int i = 0; i < int(Shape::Count); ++i)
    shapes_->add(shape_name(Shape(i)));
  shapes_->callback(choose, this);
  button(10, 404, 148, "添加所选图形", Command::SelectMode,
         [](Fl_Widget *w, void *) {
           auto *s = static_cast<DiagramWindow *>(w->window());
           int at = s->shapes_->value();
           s->add_shape(at > 0 ? Shape(at - 1) : Shape::Rounded);
         });
  caption(10, 442, 148, "对象与主题");
  objects_ = new Fl_Hold_Browser(10, 466, 148, h() - 502);
  objects_->textsize(13);
  objects_->format_char(0);
  objects_->callback(choose, this);
  left_->end();
  viewbar_ = new Fl_Group(172, 182, w() - 430, 31);
  mode_ = new Fl_Choice(172, 182, 114, 28);
  mode_->add("选择与移动|连接图形|平移画布");
  mode_->value(0);
  mode_->callback(choose, this);
  button(292, 182, 60, "适合", Command::Fit, click);
  button(358, 182, 36, "＋", Command::ZoomIn, click);
  button(400, 182, 36, "－", Command::ZoomOut, click);
  grid_ = new Fl_Check_Button(448, 182, 68, 28, "网格");
  grid_->value(1);
  grid_->callback(click, reinterpret_cast<void *>(intptr_t(Command::Grid)));
  snap_ = new Fl_Check_Button(518, 182, 68, 28, "吸附");
  snap_->value(1);
  snap_->callback(click, reinterpret_cast<void *>(intptr_t(Command::Snap)));
  viewbar_->end();
  canvas = new Canvas(172, 220, w() - 430, h() - 256, this);
  right_ = new Fl_Group(w() - 248, 181, 240, h() - 215);
  property_title_ = caption(w() - 240, 183, 224, "属性");
  properties_ = new Fl_Scroll(w() - 248, 208, 240, h() - 280);
  properties_->type(Fl_Scroll::VERTICAL_ALWAYS);
  int x = w() - 238;
  caption(x, 211, 214, "文字 / 连线标签");
  text_ = new Fl_Multiline_Input(x, 234, 208, 73);
  text_->maximum_size(4096);
  text_->wrap(1);
  text_->textsize(15);
  node_fields_ = new Fl_Group(x, 313, 214, 420);
  caption(x, 314, 62, "图形");
  shape_ = new Fl_Choice(x + 64, 312, 144, 28);
  for (int i = 0; i < int(Shape::Count); ++i)
    shape_->add(shape_name(Shape(i)));
  caption(x, 350, 214, "备注（保存在原生文档）");
  note_ = new Fl_Multiline_Input(x, 372, 208, 64);
  note_->maximum_size(16384);
  note_->wrap(1);
  note_->textsize(14);
  caption(x, 443, 214, "填充 / 边框 / 文字颜色");
  const char *cnames[] = {"填充…", "边框…", "文字…"};
  for (int n = 0; n < 3; ++n) {
    colors_[n] = new Fl_Button(x + n * 71, 466, 66, 27, cnames[n]);
    colors_[n]->callback(color_pick, this);
  }
  caption(x, 503, 40, "字号");
  font_ = new Fl_Value_Input(x + 44, 500, 64, 28);
  font_->bounds(10, 48);
  font_->step(1);
  bold_ = new Fl_Check_Button(x + 120, 500, 84, 28, "加粗");
  const char *nlabels[] = {"X", "Y", "宽", "高"};
  for (int n = 0; n < 4; ++n) {
    int col = n % 2, row = n / 2;
    caption(x + col * 108, 542 + row * 36, 24, nlabels[n]);
    numbers_[n] =
        new Fl_Value_Input(x + 24 + col * 108, 538 + row * 36, 76, 28);
    numbers_[n]->step(1);
    numbers_[n]->bounds(n < 2 ? -100000 : 40, n < 2 ? 100000 : 2000);
  }
  caption(x, 614, 208, "多选时样式应用到同类对象");
  caption(x, 636, 208, "文字与位置仅修改首个对象");
  node_fields_->end();
  edge_fields_ = new Fl_Group(x, 313, 214, 220);
  caption(x, 316, 62, "线型");
  route_ = new Fl_Choice(x + 64, 312, 144, 28);
  route_->add("直线|折线|曲线");
  caption(x, 354, 62, "箭头");
  arrows_ = new Fl_Choice(x + 64, 350, 144, 28);
  arrows_->add("无箭头|终点箭头|双向箭头");
  caption(x, 392, 62, "线宽");
  width_ = new Fl_Value_Input(x + 64, 388, 144, 28);
  width_->bounds(1, 6);
  width_->step(1);
  auto *line_color = new Fl_Button(x, 430, 208, 28, "连线颜色…");
  line_color->callback(color_pick, this);
  caption(x, 470, 208, "连接模式：依次点击两个图形");
  edge_fields_->end();
  properties_->end();
  apply_ = new Fl_Button(x, h() - 68, 208, 30, "应用属性  Ctrl+Enter");
  apply_->callback(click, reinterpret_cast<void *>(intptr_t(Command::Apply)));
  right_->end();
  body_->end();
  bottom_ = new Fl_Group(0, h() - 28, w(), 28);
  status_ = caption(8, h() - 25, w() - 330, "就绪");
  counts_ = caption(w() - 320, h() - 25, 312, "");
  counts_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  bottom_->end();
  end();
  Fl_Widget *fields[] = {text_,       note_,       shape_,      font_,
                         bold_,       numbers_[0], numbers_[1], numbers_[2],
                         numbers_[3], route_,      arrows_,     width_};
  for (Fl_Widget *f : fields) {
    f->when(FL_WHEN_CHANGED);
    f->callback(edit, this);
  }
  Fl_Group *groups[] = {menus_,   quick_,  body_,        left_,       right_,
                        viewbar_, bottom_, node_fields_, edge_fields_};
  for (Fl_Group *g : groups)
    g->resizable(nullptr);
  resizable(body_);
  Text error;
  if (!example(document, 0, error))
    update_status(error.c_str());
  refill();
  refresh();
  layout();
  canvas->fit();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh);
  int ww = int(fmin(w(), sw - 24)), hh = int(fmin(h(), sh - 44));
  if (ww >= 1100 && hh >= 680)
    resize(sx + (sw - ww) / 2, sy + (sh - hh) / 2, ww, hh);
  Fl::add_timeout(.03, tick, this);
}
DiagramWindow::~DiagramWindow() { Fl::remove_timeout(tick, this); }
void DiagramWindow::build_tools() {
  tools_ = new Fl_Tabs(8, 74, w() - 16, 98);
  const char *names[] = {"开始与插入", "布局与排列", "模板与文档"};
  for (int page = 0; page < 3; ++page) {
    toolpages_[page] = new Fl_Group(10, 99, w() - 20, 70, names[page]);
    if (page == 0) {
      button(18, 105, 88, "子主题", Command::Child, click);
      button(112, 105, 88, "同级主题", Command::Sibling, click);
      button(206, 105, 88, "折叠 / 展开", Command::Fold, click);
      caption(20, 143, 274, "思维导图 · Tab 子主题 · Enter 同级");
      for (int i = 0; i < 8; ++i) {
        auto *b = new Fl_Button(316 + (i % 4) * 124, 104 + (i / 4) * 31, 116,
                                26, shape_name(Shape(i)));
        b->callback(
            [](Fl_Widget *w, void *p) {
              static_cast<DiagramWindow *>(w->window())
                  ->add_shape(Shape(intptr_t(p)));
            },
            reinterpret_cast<void *>(intptr_t(i)));
      }
      button(826, 105, 98, "连接图形", Command::ConnectMode, click);
      button(930, 105, 98, "创建副本", Command::Duplicate, click);
      button(826, 136, 98, "删除", Command::Delete, click);
      button(930, 136, 98, "选择工具", Command::SelectMode, click);
    } else if (page == 1) {
      const char *labels[] = {"平衡导图", "向右树形", "流程分层", "左对齐",
                              "水平居中", "顶端对齐", "垂直居中", "水平分布",
                              "垂直分布", "置于顶层", "置于底层", "适合内容"};
      Command commands[] = {
          Command::MindLayout,  Command::TreeLayout,  Command::FlowLayout,
          Command::AlignLeft,   Command::AlignCenter, Command::AlignTop,
          Command::AlignMiddle, Command::DistributeH, Command::DistributeV,
          Command::Front,       Command::Back,        Command::Fit};
      for (int i = 0; i < 12; ++i)
        button(18 + (i % 6) * 164, 104 + (i / 6) * 31, 154, labels[i],
               commands[i], click);
    } else {
      button(18, 106, 166, "项目规划 · 思维导图", Command::TemplateMind, click);
      button(194, 106, 166, "审批流程 · 流程图", Command::TemplateFlow, click);
      button(370, 106, 166, "系统逻辑 · 关系图", Command::TemplateLogic, click);
      button(552, 106, 132, "文档标题…", Command::Title, click);
      button(696, 106, 142, "Markdown 大纲", Command::ExportOutline, click);
      button(850, 106, 142, "使用说明", Command::Help, click);
      caption(20, 143, 1000,
              "模板创建新文档；原生文档保留全部图形、分支、备注和样式。SVG / "
              "PNG 导出当前展开的内容。");
    }
    toolpages_[page]->end();
    toolpages_[page]->resizable(nullptr);
  }
  tools_->end();
  tools_->value(toolpages_[0]);
}
void DiagramWindow::layout() {
  menus_->resize(0, 0, w(), 32);
  title_->resize(418, 5, w() - 430, 20);
  quick_->resize(0, 35, w(), 32);
  tools_->resize(8, 74, w() - 16, 98);
  for (auto *p : toolpages_)
    p->resize(10, 99, w() - 20, 70);
  body_->resize(0, 180, w(), h() - 210);
  left_->resize(8, 181, 152, h() - 215);
  objects_->resize(10, 466, 148, h() - 502);
  viewbar_->resize(172, 182, w() - 430, 31);
  canvas->resize(172, 220, w() - 430, h() - 256);
  right_->resize(w() - 248, 181, 240, h() - 215);
  properties_->resize(w() - 248, 208, 240, h() - 280);
  apply_->resize(w() - 238, h() - 68, 208, 30);
  bottom_->resize(0, h() - 28, w(), 28);
  status_->resize(8, h() - 25, w() - 330, 20);
  counts_->resize(w() - 320, h() - 25, 312, 20);
}
void DiagramWindow::resize(int x, int y, int width, int height) {
  Fl_Double_Window::resize(x, y, width, height);
  if (canvas)
    layout();
}
void DiagramWindow::click(Fl_Widget *w, void *p) {
  static_cast<DiagramWindow *>(w->window())->run(Command(intptr_t(p)));
}
void DiagramWindow::edit(Fl_Widget *, void *p) {
  auto *s = static_cast<DiagramWindow *>(p);
  if (!s->loading_) {
    s->fields_dirty_ = true;
    s->update_status("属性已修改，点击应用或按 Ctrl+Enter");
  }
}
void DiagramWindow::color_pick(Fl_Widget *w, void *p) {
  auto *s = static_cast<DiagramWindow *>(p);
  int index = 1;
  for (int i = 0; i < 3; ++i)
    if (w == s->colors_[i])
      index = i;
  uint32_t old = s->color_values_[index];
  double r = (old >> 16) / 255.0, g = ((old >> 8) & 255) / 255.0,
         b = (old & 255) / 255.0;
  if (fl_color_chooser("选择图形颜色", r, g, b)) {
    s->color_values_[index] = (uint32_t(round(r * 255)) << 16) |
                              (uint32_t(round(g * 255)) << 8) |
                              uint32_t(round(b * 255));
    s->fields_dirty_ = true;
    s->update_status("颜色已修改，点击应用");
  }
}
void DiagramWindow::choose(Fl_Widget *w, void *p) {
  auto *s = static_cast<DiagramWindow *>(p);
  if (w == s->objects_) {
    int at = s->objects_->value();
    if (at > 0 && size_t(at) <= s->rows_.size())
      s->select(s->rows_[size_t(at - 1)]);
  } else if (w == s->shapes_ && Fl::event_clicks()) {
    int at = s->shapes_->value();
    if (at > 0)
      s->add_shape(Shape(at - 1));
  } else if (w == s->mode_) {
    s->canvas->cancel();
    s->update_status(s->mode() == 1 ? "连接模式：依次点击起点和终点；Esc 取消"
                     : s->mode() == 2
                         ? "拖动画布平移；滚轮缩放"
                         : "拖动图形移动；Shift 多选；拖动空白框选");
  }
}
void DiagramWindow::refill() {
  rows_.clear();
  objects_->clear();
  for (size_t i = 0; i < document.nodes.size(); ++i) {
    const Node &n = document.nodes[i];
    char buf[256];
    int depth = 0;
    uint64_t p = n.parent;
    while (p && depth < 8) {
      const Node *a = node(document, p);
      if (!a)
        break;
      ++depth;
      p = a->parent;
    }
    char clean[160];
    size_t used = 0;
    for (size_t j = 0; j < n.text.size();) {
      unsigned char c = n.text.c_str()[j];
      size_t length = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
      if (used + length >= sizeof(clean))
        break;
      if (c == '\n')
        clean[used++] = ' ';
      else {
        memcpy(clean + used, n.text.c_str() + j, length);
        used += length;
      }
      j += length;
    }
    clean[used] = 0;
    snprintf(buf, sizeof(buf), "%*s%s%s", depth * 2, "",
             n.collapsed ? "＋ " : "", clean);
    if (rows_.push(n.id))
      objects_->add(buf);
  }
  for (size_t i = 0; i < document.edges.size(); ++i) {
    char buf[192];
    snprintf(buf, sizeof(buf), "↳ %s",
             document.edges[i].text.empty() ? "连线"
                                            : document.edges[i].text.c_str());
    if (rows_.push(document.edges[i].id))
      objects_->add(buf);
  }
  if (!selection.empty())
    for (size_t i = 0; i < rows_.size(); ++i)
      if (rows_[i] == selection[0])
        objects_->select(int(i + 1));
}
void DiagramWindow::refresh() {
  loading_ = true;
  fields_dirty_ = false;
  Node *n = selection.empty() ? nullptr : node(document, selection[0]);
  Edge *a = selection.empty() ? nullptr : edge(document, selection[0]);
  if (!n && !a) {
    properties_->deactivate();
    apply_->deactivate();
    node_fields_->hide();
    edge_fields_->hide();
    text_->value("");
    properties_->redraw();
    property_title_->copy_label("属性 · 请选择图形或连线");
    loading_ = false;
    return;
  }
  properties_->activate();
  apply_->activate();
  property_title_->copy_label(selection.size() > 1 ? "属性 · 多选"
                                                   : "属性 · 当前对象");
  text_->maximum_size(n ? 4096 : 1024);
  text_->value(n ? n->text.c_str() : a->text.c_str());
  if (n) {
    node_fields_->show();
    edge_fields_->hide();
    shape_->value(int(n->shape));
    note_->value(n->note.c_str());
    color_values_[0] = n->style.fill;
    color_values_[1] = n->style.line;
    color_values_[2] = n->style.text;
    font_->value(n->style.font);
    bold_->value(n->style.bold);
    double values[] = {n->x, n->y, n->w, n->h};
    for (int i = 0; i < 4; ++i)
      numbers_[i]->value(values[i]);
  } else {
    node_fields_->hide();
    edge_fields_->show();
    route_->value(int(a->route));
    arrows_->value(a->arrows);
    width_->value(a->width);
    color_values_[1] = a->color;
  }
  loading_ = false;
  properties_->redraw();
}
bool DiagramWindow::selected(uint64_t id) const {
  for (size_t i = 0; i < selection.size(); ++i)
    if (selection[i] == id)
      return true;
  return false;
}
bool DiagramWindow::select(uint64_t id, bool toggle) {
  if (!apply_fields())
    return false;
  if (!toggle)
    selection.clear();
  if (id) {
    for (size_t i = 0; i < selection.size(); ++i)
      if (selection[i] == id) {
        if (toggle)
          selection.erase(i);
        selection_changed();
        return true;
      }
    if (!selection.push(id)) {
      update_status("选择内存不足");
      return false;
    }
  }
  selection_changed();
  return true;
}
void DiagramWindow::selection_changed() {
  refresh();
  refill();
  canvas->redraw();
  update_status();
}
bool DiagramWindow::commit(Document &&next) {
  Text err;
  if (!validate(next, err)) {
    fl_alert("%s", err.c_str());
    return false;
  }
  if (!history_.checkpoint(document)) {
    fl_alert("内存不足，无法保存撤销记录");
    return false;
  }
  document = static_cast<Document &&>(next);
  dirty_ = true;
  fields_dirty_ = false;
  changed();
  return true;
}
bool DiagramWindow::begin_gesture() {
  if (!apply_fields())
    return false;
  if (!history_.checkpoint(document)) {
    update_status("内存不足，无法移动");
    return false;
  }
  return true;
}
void DiagramWindow::finish_gesture() {
  dirty_ = true;
  changed();
}
void DiagramWindow::changed(bool rebuild_list) {
  for (size_t i = selection.size(); i > 0; --i)
    if (!node(document, selection[i - 1]) && !edge(document, selection[i - 1]))
      selection.erase(i - 1);
  canvas->invalidate();
  if (rebuild_list)
    refill();
  refresh();
  update_status();
}
bool DiagramWindow::apply_fields() {
  if (!fields_dirty_)
    return true;
  if (selection.empty()) {
    fields_dirty_ = false;
    return true;
  }
  Document next;
  if (!next.clone_from(document)) {
    fl_alert("内存不足");
    return false;
  }
  uint64_t first = selection[0];
  if (Node *n = node(next, first)) {
    if (!isfinite(font_->value()) || font_->value() < 10 ||
        font_->value() > 48) {
      fl_alert("字号必须在 10 到 48 之间");
      return false;
    }
    if (!n->text.set(text_->value()) || !n->note.set(note_->value()))
      return false;
    n->shape = Shape(shape_->value());
    n->x = numbers_[0]->value();
    n->y = numbers_[1]->value();
    n->w = numbers_[2]->value();
    n->h = numbers_[3]->value();
    for (size_t i = 0; i < selection.size(); ++i)
      if (Node *p = node(next, selection[i])) {
        p->style = {color_values_[0], color_values_[1], color_values_[2],
                    int(font_->value()), bool(bold_->value())};
      }
  } else if (Edge *a = edge(next, first)) {
    if (!isfinite(width_->value()) || width_->value() < 1 ||
        width_->value() > 6) {
      fl_alert("线宽必须在 1 到 6 之间");
      return false;
    }
    if (!a->text.set(text_->value()))
      return false;
    for (size_t i = 0; i < selection.size(); ++i)
      if (Edge *p = edge(next, selection[i])) {
        p->route = Route(route_->value());
        p->arrows = unsigned(arrows_->value());
        p->width = unsigned(width_->value());
        p->color = color_values_[1];
      }
  }
  bool ok = commit(static_cast<Document &&>(next));
  if (ok)
    update_status("属性已应用");
  return ok;
}
void DiagramWindow::update_status(const char *message) {
  if (message)
    notice_.set(message);
  const char *text =
      busy()             ? (operation_ == Operation::Png ? "正在后台编码 PNG…"
                                                         : "正在后台读写文件…")
      : !notice_.empty() ? notice_.c_str()
                         : "拖动移动 · Shift 多选 · 滚轮缩放 · 双击编辑文字";
  status_->copy_label(text);
  char buf[512];
  snprintf(buf, sizeof(buf), "%zu 个图形  %zu 条连线  选中 %zu  %.0f%%",
           document.nodes.size(), document.edges.size(), selection.size(),
           document.view.zoom * 100);
  counts_->copy_label(buf);
  const char *name =
      path_.empty() ? document.title.c_str() : strrchr(path_.c_str(), '/');
  if (!path_.empty())
    name = name ? name + 1 : path_.c_str();
  if (!name || !*name)
    name = "未命名";
  snprintf(buf, sizeof(buf), "%s%s — ReliefDiagram",
           dirty_ || fields_dirty_ ? "* " : "", name);
  copy_label(buf);
  grid_->value(document.view.grid);
  snap_->value(document.view.snap);
}
void DiagramWindow::tick(void *p) {
  auto *s = static_cast<DiagramWindow *>(p);
  s->poll_io();
  if (s->operation_ == Operation::Png && s->png_.poll()) {
    Text e;
    if (s->png_.take(s->pending_bytes_, e)) {
      s->operation_ = Operation::Idle;
      s->start_write(s->pending_path_.c_str(), true);
    } else {
      s->operation_ = Operation::Idle;
      s->busy_ui(false);
      fl_alert("%s", e.c_str());
    }
  }
  Fl::repeat_timeout(.03, tick, p);
}
} // namespace rd
