#include "window.h"
#include "analysis.h"
#include <FL/Fl_Menu_Button.H>
#include <errno.h>
#include <math.h>
#include <stdio.h>
namespace rmp {
namespace {
void label(Fl_Box *b) {
  b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  b->labelsize(13);
}
Fl_Box *caption(int x, int y, int w, const char *text) {
  auto *b = new Fl_Box(x, y, w, 19, text);
  label(b);
  return b;
}
void numbers(char *buf, size_t capacity, const double *values, int count) {
  size_t used = 0;
  *buf = 0;
  for (int n = 0; n < count; ++n) {
    int written = snprintf(buf + used, capacity - used, n ? ", %.10g" : "%.10g",
                           values[n]);
    if (written < 0 || size_t(written) >= capacity - used)
      return;
    used += size_t(written);
  }
}
bool parse_numbers(const char *s, double *out, int count) {
  for (int n = 0; n < count; ++n) {
    while (*s == ' ' || *s == '\t')
      ++s;
    char *end;
    errno = 0;
    double x = strtod(s, &end);
    if (end == s || errno == ERANGE || !isfinite(x))
      return false;
    out[n] = x;
    s = end;
    while (*s == ' ' || *s == '\t')
      ++s;
    if (n < count - 1) {
      if (*s++ != ',')
        return false;
    }
  }
  return !*s;
}
} // namespace
MathWindow::MathWindow() : Fl_Double_Window(1200, 780, "ReliefMath++") {
  size_range(1000, 680);
  callback([](Fl_Widget *,
              void *p) { static_cast<MathWindow *>(p)->run(Command::Close); },
           this);
  begin();
  top_ = new Fl_Group(0, 0, w(), 32);
  const char *menus[] = {"文件", "编辑", "视图", "示例", "帮助"};
  Fl_Menu_Button *m[5];
  for (int n = 0; n < 5; ++n)
    m[n] = new Fl_Menu_Button(8 + n * 76, 2, 70, 28, menus[n]);
  auto menu = [&](int n, const char *name, Command command, int shortcut = 0) {
    m[n]->add(name, shortcut, click,
              reinterpret_cast<void *>(intptr_t(command)));
  };
  menu(0, "新建几何文档", Command::New, FL_CTRL + 'n');
  menu(0, "打开…", Command::Open, FL_CTRL + 'o');
  menu(0, "保存", Command::Save, FL_CTRL + 's');
  menu(0, "另存为…", Command::SaveAs, FL_CTRL + FL_SHIFT + 's');
  menu(0, "导出当前视图 SVG…", Command::ExportSvg);
  menu(0, "导出当前视图 PNG…", Command::ExportPng);
  menu(0, "函数值表与 CSV…", Command::Table);
  menu(0, "退出", Command::Close, FL_ALT + FL_F + 4);
  menu(1, "撤销", Command::Undo, FL_CTRL + 'z');
  menu(1, "重做", Command::Redo, FL_CTRL + 'y');
  menu(1, "复制对象", Command::Duplicate, FL_CTRL + 'd');
  menu(1, "删除对象", Command::Delete);
  menu(1, "显示/隐藏对象", Command::Toggle);
  menu(1, "应用属性", Command::Apply, FL_CTRL + FL_Enter);
  menu(2, "重置视图", Command::Reset, FL_Home);
  menu(2, "适合全部对象", Command::Fit);
  menu(2, "放大", Command::ZoomIn);
  menu(2, "缩小", Command::ZoomOut);
  menu(2, "网格", Command::Grid);
  menu(2, "坐标轴", Command::Axes);
  menu(2, "三维线框", Command::Wire);
  menu(3, "正弦、余弦与圆", Command::Example2);
  menu(3, "波纹曲面与螺旋线", Command::Example3);
  menu(3, "环面与空间向量", Command::ExampleTorus);
  menu(3, "玫瑰线与椭圆", Command::ExamplePolar);
  menu(4, "使用说明与表达式参考", Command::Help, FL_F + 1);
  auto *title = caption(422, 5, 730, "ReliefMath++   几何 · 函数 · 数值计算");
  title->labelsize(16);
  top_->end();
  quick_ = new Fl_Group(0, 34, w(), 31);
  const char *qnames[] = {"新建",     "打开",     "保存",     "撤销",    "重做",
                          "复制对象", "删除对象", "SVG 导出", "PNG 导出"};
  const Command commands[] = {
      Command::New,    Command::Open,      Command::Save,
      Command::Undo,   Command::Redo,      Command::Duplicate,
      Command::Delete, Command::ExportSvg, Command::ExportPng};
  int at = 8;
  for (int n = 0; n < 9; ++n) {
    int width = n < 5 ? 65 : 94;
    auto *b = new Fl_Button(at, 35, width, 28, qnames[n]);
    b->callback(click, reinterpret_cast<void *>(intptr_t(commands[n])));
    at += width + 6;
  }
  quick_->end();
  build_tools();
  body_ = new Fl_Group(0, 170, w(), h() - 200);
  left_ = new Fl_Group(8, 171, 350, h() - 207);
  caption(10, 172, 340, "对象清单（选中后编辑；勾选可见性）");
  list_ = new Fl_Hold_Browser(10, 195, 344, 137);
  list_->textsize(14);
  list_->format_char(0);
  list_->callback(choose, this);
  properties_ = new Fl_Scroll(8, 340, 350, h() - 374);
  properties_->type(Fl_Scroll::VERTICAL_ALWAYS);
  caption(16, 342, 320, "对象属性");
  caption(16, 365, 46, "名称");
  name_ = new Fl_Input(64, 363, 270, 27);
  name_->maximum_size(128);
  caption(16, 399, 46, "类型");
  type_ = new Fl_Choice(64, 397, 270, 27);
  for (int n = 0; n < int(Kind::Count); ++n)
    type_->add(info(Kind(n)).name);
  type_->callback(choose, this);
  hint_ = caption(16, 430, 318, "");
  hint_->labelsize(12);
  for (int n = 0; n < 3; ++n) {
    formula_labels_[n] = caption(16, 456 + n * 34, 46,
                                 n == 0   ? "F1"
                                 : n == 1 ? "F2"
                                          : "F3");
    formula_[n] = new Fl_Input(64, 454 + n * 34, 270, 28);
    formula_[n]->maximum_size(1024);
    formula_[n]->textsize(14);
  }
  coord_label_ = caption(16, 558, 318, "坐标 / 尺寸（逗号分隔）");
  coords_ = new Fl_Input(16, 579, 318, 29);
  coords_->maximum_size(400);
  coords_->textsize(14);
  range_labels_[0] = caption(16, 615, 106, "X / t / u 范围");
  ranges_[0] = new Fl_Input(122, 613, 212, 27);
  range_labels_[1] = caption(16, 649, 106, "Y / v 范围");
  ranges_[1] = new Fl_Input(122, 647, 212, 27);
  color_label_ = caption(16, 683, 46, "颜色");
  color_ = new Fl_Choice(64, 681, 112, 27);
  color_->add("蓝|红|绿|紫|金|青|粉|灰");
  quality_label_ = caption(184, 683, 46, "采样");
  quality_ = new Fl_Choice(230, 681, 104, 27);
  quality_->add("24|32|48|64|96");
  visible_ = new Fl_Check_Button(16, 717, 104, 28, "可见");
  apply_ = new Fl_Button(128, 716, 206, 30, "应用属性  Ctrl+Enter");
  apply_->callback(click, reinterpret_cast<void *>(intptr_t(Command::Apply)));
  tip_ = caption(16, 754, 318, "提示：公式乘法用 *；角度统一为弧度");
  properties_->end();
  left_->end();
  right_ = new Fl_Group(370, 171, w() - 380, h() - 205);
  viewbar_ = new Fl_Group(370, 171, w() - 380, 30);
  const char *vnames[] = {"重置", "适合", "＋", "－"};
  const Command vc[] = {Command::Reset, Command::Fit, Command::ZoomIn,
                        Command::ZoomOut};
  for (int n = 0; n < 4; ++n) {
    auto *b = new Fl_Button(370 + n * 58, 171, 52, 28, vnames[n]);
    b->callback(click, reinterpret_cast<void *>(intptr_t(vc[n])));
  }
  grid_ = new Fl_Check_Button(608, 171, 70, 28, "网格");
  axes_ = new Fl_Check_Button(680, 171, 70, 28, "坐标轴");
  wire_ = new Fl_Check_Button(758, 171, 66, 28, "线框");
  trace_ = new Fl_Check_Button(830, 171, 82, 28, "追踪");
  Fl_Check_Button *checks[] = {grid_, axes_, wire_, trace_};
  Command cc[] = {Command::Grid, Command::Axes, Command::Wire, Command::Trace};
  for (int n = 0; n < 4; ++n)
    checks[n]->callback(click, reinterpret_cast<void *>(intptr_t(cc[n])));
  viewbar_->end();
  views_ = new Fl_Tabs(370, 208, w() - 380, h() - 324);
  for (int n = 0; n < 2; ++n) {
    viewpages_[n] = new Fl_Group(372, 233, w() - 384, h() - 351,
                                 n ? "三维几何 / 曲面" : "二维几何 / 函数");
    canvas[n] = new Canvas(374, 235, w() - 388, h() - 355, this, n);
    viewpages_[n]->end();
    viewpages_[n]->resizable(canvas[n]);
  }
  views_->end();
  views_->callback(
      [](Fl_Widget *w, void *p) {
        auto *s = static_cast<MathWindow *>(p);
        s->select_tab(
            static_cast<Fl_Tabs *>(w)->value() == s->viewpages_[1] ? 1 : 0);
      },
      this);
  params_ = new Fl_Group(370, h() - 107, w() - 380, 73);
  for (int n = 0; n < 4; ++n) {
    int col = n % 2, row = n / 2, x = 376 + col * (w() - 390) / 2,
        y = h() - 104 + row * 34;
    caption(x, y + 4, 20, n == 0 ? "a" : n == 1 ? "b" : n == 2 ? "c" : "d");
    sliders_[n] = new Fl_Value_Slider(x + 24, y, (w() - 390) / 2 - 116, 27);
    sliders_[n]->type(FL_HOR_NICE_SLIDER);
    sliders_[n]->bounds(-5, 5);
    sliders_[n]->step(.05);
    sliders_[n]->value(1);
    sliders_[n]->callback(parameter_changed, this);
    parameter_[n] = new Fl_Input(x + (w() - 390) / 2 - 84, y, 75, 27);
    parameter_[n]->value("1");
    parameter_[n]->when(FL_WHEN_ENTER_KEY_ALWAYS);
    parameter_[n]->callback(parameter_changed, this);
  }
  params_->end();
  right_->end();
  body_->end();
  bottom_ = new Fl_Group(0, h() - 28, w(), 28);
  status_ = caption(8, h() - 26, w() - 420, "就绪");
  counts_ = caption(w() - 410, h() - 26, 402, "");
  counts_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  bottom_->end();
  end();
  Fl_Widget *fields[] = {name_,    formula_[0], formula_[1], formula_[2],
                         coords_,  ranges_[0],  ranges_[1],  color_,
                         quality_, visible_};
  for (Fl_Widget *field : fields) {
    field->when(FL_WHEN_CHANGED);
    field->callback(edit, this);
  }
  Fl_Group *groups[] = {top_,   quick_,   tools_,  body_,  left_,
                        right_, viewbar_, params_, bottom_};
  for (Fl_Group *g : groups)
    g->resizable(nullptr);
  resizable(body_);
  example(document, 0);
  active_ = document.objects[0].id;
  refill();
  refresh();
  select_tab(0);
  resample();
  layout();
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh);
  int ww = w() < sw - 24 ? w() : sw - 24, hh = h() < sh - 44 ? h() : sh - 44;
  if (ww >= 1000 && hh >= 680)
    resize(sx + (sw - ww) / 2, sy + (sh - hh) / 2, ww, hh);
  Fl::add_timeout(.03, tick, this);
}
MathWindow::~MathWindow() { Fl::remove_timeout(tick, this); }
void MathWindow::build_tools() {
  tools_ = new Fl_Tabs(8, 70, w() - 16, 92);
  const char *tabs[] = {"二维绘图", "三维绘图", "计算与工具"};
  for (int page = 0; page < 3; ++page) {
    toolpages_[page] = new Fl_Group(10, 95, w() - 20, 64, tabs[page]);
    if (page < 2) {
      int first = page ? int(Kind::Function3) : 0,
          last = page ? int(Kind::Count) : int(Kind::Function3);
      int n = 0;
      for (int kind = first; kind < last; ++kind, ++n) {
        auto *b = new Fl_Button(16 + (n % 7) * 136, 99 + (n / 7) * 29, 128, 25,
                                info(Kind(kind)).name);
        b->callback(
            [](Fl_Widget *w, void *v) {
              static_cast<MathWindow *>(w->window())
                  ->add_object(Kind(intptr_t(v)));
            },
            reinterpret_cast<void *>(intptr_t(kind)));
        b->tooltip(info(Kind(kind)).help);
      }
    } else {
      const char *names[] = {"函数计算…",    "函数值表…",    "几何度量",
                             "向量 / 两点…", "拾点：点",     "拾点：线段",
                             "拾点：圆",     "拾点：三角形", "拾点：向量",
                             "取消拾点",     "表达式帮助",   "三维函数计算…"};
      Command cmd[] = {
          Command::Calculate,  Command::Table,        Command::Measure,
          Command::Vectors,    Command::PickPoint,    Command::PickSegment,
          Command::PickCircle, Command::PickTriangle, Command::PickVector,
          Command::CancelPick, Command::Help,         Command::Surface};
      static_assert(sizeof(names) / sizeof(names[0]) ==
                        sizeof(cmd) / sizeof(cmd[0]),
                    "tool names must match actions");
      for (size_t n = 0; n < sizeof(cmd) / sizeof(cmd[0]); ++n) {
        auto *b = new Fl_Button(16 + (n % 7) * 136, 99 + (n / 7) * 29, 128, 25,
                                names[n]);
        b->callback(click, reinterpret_cast<void *>(intptr_t(cmd[n])));
      }
    }
    toolpages_[page]->end();
    toolpages_[page]->resizable(nullptr);
  }
  tools_->end();
  tools_->value(toolpages_[0]);
}
void MathWindow::layout() {
  top_->resize(0, 0, w(), 32);
  quick_->resize(0, 34, w(), 31);
  tools_->resize(8, 70, w() - 16, 92);
  for (auto *g : toolpages_)
    g->resize(10, 95, w() - 20, 64);
  body_->resize(0, 170, w(), h() - 200);
  left_->resize(8, 171, 350, h() - 207);
  properties_->resize(8, 340, 350, h() - 374);
  right_->resize(370, 171, w() - 380, h() - 205);
  viewbar_->resize(370, 171, w() - 380, 30);
  views_->resize(370, 208, w() - 380, h() - 324);
  for (int n = 0; n < 2; ++n) {
    viewpages_[n]->resize(372, 233, w() - 384, h() - 351);
    canvas[n]->resize(374, 235, w() - 388, h() - 355);
  }
  params_->resize(370, h() - 107, w() - 380, 73);
  // Move individual slider rows explicitly; the standard group does not impose
  // a theme or layout.
  int width = (w() - 390) / 2;
  for (int n = 0; n < 4; ++n) {
    int x = 376 + (n % 2) * width, y = h() - 104 + (n / 2) * 34;
    params_->child(n * 3)->resize(x, y + 4, 20, 19);
    sliders_[n]->resize(x + 24, y, width - 116, 27);
    parameter_[n]->resize(x + width - 84, y, 75, 27);
  }
  bottom_->resize(0, h() - 28, w(), 28);
  status_->resize(8, h() - 26, w() - 420, 19);
  counts_->resize(w() - 410, h() - 26, 402, 19);
  invalidate();
}
void MathWindow::layout_properties() {
  properties_->scroll_to(0, 0);
  Kind k = Kind(type_->value());
  const KindInfo &type = info(k);
  int y = 454;
  const char *names[3] = {"y(x)", "y(t)", "z(t)"};
  if (k == Kind::Function3)
    names[0] = "z(x,y)";
  if (k == Kind::Implicit2)
    names[0] = "F(x,y)";
  if (k == Kind::Polar2)
    names[0] = "r(t)";
  if (k == Kind::Parametric2 || k == Kind::Curve3)
    names[0] = "x(t)";
  if (k == Kind::Parametric3) {
    names[0] = "x(u,v)";
    names[1] = "y(u,v)";
    names[2] = "z(u,v)";
  }
  for (int n = 0; n < 3; ++n) {
    if (n < type.formulas) {
      formula_labels_[n]->show();
      formula_[n]->show();
      formula_labels_[n]->copy_label(names[n]);
      formula_labels_[n]->resize(16, y + 2, 46, 19);
      formula_[n]->resize(64, y, 270, 28);
      y += 34;
    } else {
      formula_labels_[n]->hide();
      formula_[n]->hide();
    }
  }
  if (type.values) {
    coord_label_->show();
    coords_->show();
    coord_label_->resize(16, y, 318, 19);
    coords_->resize(16, y + 23, 318, 29);
    y += 62;
  } else {
    coord_label_->hide();
    coords_->hide();
  }
  bool ranges = type.formulas || k == Kind::Plane3;
  for (int n = 0; n < 2; ++n) {
    if (ranges) {
      range_labels_[n]->show();
      ranges_[n]->show();
      range_labels_[n]->resize(16, y + 2, 106, 19);
      ranges_[n]->resize(122, y, 212, 27);
      y += 34;
    } else {
      range_labels_[n]->hide();
      ranges_[n]->hide();
    }
  }
  color_label_->resize(16, y + 2, 46, 19);
  color_->resize(64, y, 112, 27);
  quality_label_->resize(184, y + 2, 46, 19);
  quality_->resize(230, y, 104, 27);
  y += 36;
  visible_->resize(16, y, 104, 28);
  apply_->resize(128, y, 206, 30);
  y += 38;
  tip_->resize(16, y, 318, 19);
  properties_->redraw();
}
void MathWindow::resize(int x, int y, int width, int height) {
  Fl_Double_Window::resize(x, y, width, height);
  layout();
}
void MathWindow::click(Fl_Widget *w, void *p) {
  static_cast<MathWindow *>(w->window())->run(Command(intptr_t(p)));
}
void MathWindow::edit(Fl_Widget *, void *p) {
  auto *s = static_cast<MathWindow *>(p);
  if (s->loading_)
    return;
  s->fields_dirty_ = true;
  s->update_status("属性尚未应用；Ctrl+Enter 应用");
}
void MathWindow::choose(Fl_Widget *w, void *p) {
  auto *s = static_cast<MathWindow *>(p);
  if (s->loading_)
    return;
  if (w == s->list_) {
    int row = s->list_->value();
    if (!s->apply_fields()) {
      s->refill();
      return;
    }
    if (row > 0 && size_t(row) <= s->document.objects.size()) {
      s->active_ = s->document.objects[size_t(row - 1)].id;
      s->refresh();
      s->select_tab(info(s->current()->kind).dim3 ? 1 : 0);
    }
  } else {
    Object o = make_object(Kind(s->type_->value()));
    Text name;
    name.set(s->name_->value());
    o.name = name;
    s->loading_ = true;
    for (int n = 0; n < 3; ++n)
      s->formula_[n]->value(o.formula[n].c_str());
    char b[400];
    numbers(b, sizeof(b), o.p, info(o.kind).values);
    s->coords_->value(b);
    for (int n = 0; n < 2; ++n) {
      numbers(b, sizeof(b), o.range + 2 * n, 2);
      s->ranges_[n]->value(b);
    }
    s->hint_->copy_label(info(o.kind).help);
    s->coord_label_->copy_label(info(o.kind).fields);
    for (int n = 0; n < 3; ++n) {
      if (n < info(o.kind).formulas)
        s->formula_[n]->activate();
      else
        s->formula_[n]->deactivate();
    }
    if (info(o.kind).values)
      s->coords_->activate();
    else
      s->coords_->deactivate();
    s->loading_ = false;
    s->layout_properties();
    s->fields_dirty_ = true;
    s->update_status("类型已选择；应用后更新对象");
  }
}
void MathWindow::refill() {
  loading_ = true;
  list_->clear();
  for (size_t n = 0; n < document.objects.size(); ++n) {
    const Object &o = document.objects[n];
    char b[256];
    snprintf(b, sizeof(b), "%s %s  %s", o.visible ? "●" : "○",
             info(o.kind).dim3 ? "3D" : "2D", o.name.c_str());
    list_->add(b);
    if (o.id == active_)
      list_->value(int(n + 1));
  }
  loading_ = false;
}
void MathWindow::refresh() {
  Object *o = current();
  if (!o) {
    properties_->deactivate();
    return;
  }
  properties_->activate();
  loading_ = true;
  name_->value(o->name.c_str());
  type_->value(int(o->kind));
  hint_->copy_label(info(o->kind).help);
  hint_->tooltip(info(o->kind).help);
  const char *labels2[] = {"y(x)", "y(t)", "z(t)"};
  const char *labels3[] = {"x(u,v)", "y(u,v)", "z(u,v)"};
  for (int n = 0; n < 3; ++n) {
    formula_[n]->value(o->formula[n].c_str());
    formula_labels_[n]->labelsize(12);
    formula_labels_[n]->copy_label(o->kind == Kind::Parametric3 ? labels3[n]
                                   : o->kind == Kind::Parametric2 ||
                                           o->kind == Kind::Curve3
                                       ? (n == 0 ? "x(t)" : labels2[n])
                                   : o->kind == Kind::Function3 ? "z(x,y)"
                                   : o->kind == Kind::Implicit2 ? "F(x,y)"
                                   : o->kind == Kind::Polar2    ? "r(t)"
                                                                : "y(x)");
    if (n < info(o->kind).formulas)
      formula_[n]->activate();
    else
      formula_[n]->deactivate();
  }
  char b[400];
  numbers(b, sizeof(b), o->p, info(o->kind).values);
  coords_->value(b);
  coord_label_->copy_label(info(o->kind).values
                               ? info(o->kind).fields
                               : "坐标 / 尺寸（此类型使用公式）");
  if (info(o->kind).values)
    coords_->activate();
  else
    coords_->deactivate();
  for (int n = 0; n < 2; ++n) {
    numbers(b, sizeof(b), o->range + 2 * n, 2);
    ranges_[n]->value(b);
  }
  color_->value(o->color);
  int quality[] = {24, 32, 48, 64, 96}, q = 0;
  for (int n = 0; n < 5; ++n)
    if (o->resolution == quality[n])
      q = n;
  quality_->value(q);
  visible_->value(o->visible);
  fields_dirty_ = false;
  layout_properties();
  loading_ = false;
  update_status();
}
bool MathWindow::checkpoint() {
  if (history_.checkpoint(document))
    return true;
  fl_alert("内存不足，不能记录撤销历史");
  return false;
}
bool MathWindow::apply_fields() {
  if (!fields_dirty_)
    return true;
  Object *old = current();
  if (!old)
    return true;
  Object o = *old;
  o.kind = Kind(type_->value());
  Text error;
  if (!o.name.set(name_->value()))
    return false;
  for (int n = 0; n < 3; ++n)
    if (!o.formula[n].set(formula_[n]->value()))
      return false;
  if (!parse_numbers(coords_->value(), o.p, info(o.kind).values) ||
      !parse_numbers(ranges_[0]->value(), o.range, 2) ||
      !parse_numbers(ranges_[1]->value(), o.range + 2, 2)) {
    fl_alert("坐标和范围请输入有限数值，并使用英文逗号分隔");
    return false;
  }
  int quality[] = {24, 32, 48, 64, 96};
  o.resolution = quality[quality_->value()];
  o.color = color_->value();
  o.visible = visible_->value() != 0;
  if (!valid_object(o, error)) {
    fl_alert("%s", error.c_str());
    return false;
  }
  if (!checkpoint())
    return false;
  *old = o;
  fields_dirty_ = false;
  changed();
  select_tab(info(o.kind).dim3 ? 1 : 0);
  refresh();
  return true;
}
void MathWindow::changed(bool rebuild) {
  dirty_ = true;
  refill();
  if (rebuild)
    resample();
  else
    invalidate();
  update_status();
}
void MathWindow::view_changed() {
  dirty_ = true;
  invalidate();
  update_status();
}
void MathWindow::parameter_changed(Fl_Widget *w, void *p) {
  auto *s = static_cast<MathWindow *>(p);
  if (s->loading_ || s->io_.busy())
    return;
  if (!s->apply_fields())
    return;
  double *values[] = {&s->document.vars.a, &s->document.vars.b,
                      &s->document.vars.c, &s->document.vars.d};
  for (int n = 0; n < 4; ++n)
    if (w == s->sliders_[n] || w == s->parameter_[n]) {
      double value;
      if (w == s->sliders_[n])
        value = s->sliders_[n]->value();
      else if (!parse_numbers(s->parameter_[n]->value(), &value, 1) ||
               fabs(value) > 1e8) {
        fl_alert("参数必须为绝对值不超过 1e8 的有限数值");
        return;
      }
      if (*values[n] == value)
        return;
      if (!s->checkpoint())
        return;
      *values[n] = value;
      char b[40];
      snprintf(b, sizeof(b), "%.10g", value);
      s->parameter_[n]->value(b);
      s->sliders_[n]->value(fmax(-5, fmin(5, value)));
      s->changed();
      break;
    }
}
void MathWindow::select_tab(int tab) {
  document.view.tab = tab;
  views_->value(viewpages_[tab]);
  grid_->value(document.view.grid);
  axes_->value(document.view.axes);
  wire_->value(document.view.wire);
  trace_->value(trace_mode_);
  update_status();
  invalidate();
}
void MathWindow::invalidate() {
  for (Canvas *c : canvas)
    if (c)
      c->invalidate();
}
void MathWindow::resample() {
  meshes.clear();
  sampling_ = meshes.resize(document.objects.size());
  sample_index_ = 0;
  if (sampling_ && !document.objects.empty())
    sampler_.begin(document.objects[0], document.vars, meshes[0]);
  else
    sampling_ = false;
  invalidate();
}
void MathWindow::poll_sampling() {
  if (!sampling_)
    return;
  if (sampler_.step(meshes[sample_index_], 2)) {
    ++sample_index_;
    if (sample_index_ >= document.objects.size()) {
      sampling_ = false;
      puts("[reliefmathpp.elf] sampling complete");
      fflush(stdout);
      update_status();
    } else {
      Object o = document.objects[sample_index_];
      int surfaces = 0;
      for (size_t n = 0; n < document.objects.size(); ++n)
        if (info(document.objects[n].kind).dim3)
          ++surfaces;
      int budget = int(sqrt(110000.0 / fmax(1, 2 * surfaces)));
      if (o.resolution > budget)
        o.resolution = budget < 16 ? 16 : budget;
      sampler_.begin(o, document.vars, meshes[sample_index_]);
    }
  }
  invalidate();
}
void MathWindow::update_status(const char *message) {
  if (message)
    notice_.set(message);
  else if (sampling_)
    notice_.set("正在分批采样…");
  else if (io_.busy())
    notice_.set("正在处理文件…");
  else {
    Object *o = current();
    Text err;
    if (o)
      for (size_t n = 0; n < document.objects.size() && n < meshes.size(); ++n)
        if (document.objects[n].id == o->id)
          err = meshes[n].error;
    notice_.set(err.empty() ? "就绪 · 拖动平移/旋转，滚轮缩放" : err.c_str());
  }
  if (status_)
    status_->copy_label(notice_.c_str());
  char b[192];
  snprintf(b, sizeof(b), "%zu 个对象   %s   a=%.3g   %s",
           document.objects.size(), document.view.tab ? "3D" : "2D",
           document.vars.a, dirty_ ? "未保存" : "已保存");
  if (counts_)
    counts_->copy_label(b);
  char title[512];
  snprintf(title, sizeof(title), "%s%s — ReliefMath++", dirty_ ? "* " : "",
           path_.empty() ? "未命名几何文档" : path_.c_str());
  copy_label(title);
}
void MathWindow::pointer(double x, double y) {
  if (picking())
    return;
  char b[256];
  Object *o = current();
  if (trace_mode_ && o && o->kind == Kind::Function2) {
    Expression e;
    Text err;
    Variables v = document.vars;
    v.x = x;
    if (e.compile(o->formula[0].c_str(), err))
      snprintf(b, sizeof(b), "追踪  x = %.7g    f(x) = %.7g", x, e.eval(v));
    else
      return;
  } else
    snprintf(b, sizeof(b), "坐标  x = %.7g    y = %.7g", x, y);
  status_->copy_label(b);
}
void MathWindow::add_object(Kind kind) {
  if (io_.busy() || !apply_fields())
    return;
  if (document.objects.size() >= MaxObjects) {
    fl_alert("每个文档最多 64 个对象");
    return;
  }
  if (!checkpoint())
    return;
  Object o = make_object(kind);
  o.color = int(document.objects.size() % 8);
  if (!append(document, o, active_)) {
    fl_alert("不能添加对象");
    return;
  }
  changed();
  refresh();
  select_tab(info(kind).dim3 ? 1 : 0);
}
void MathWindow::picked(Vec3 p) {
  if (!picking())
    return;
  int required = pick_kind_ == Kind::Point2      ? 1
                 : pick_kind_ == Kind::Triangle2 ? 3
                                                 : 2;
  picks_[pick_count_++] = p;
  if (pick_count_ < required) {
    update_status("继续在画布上选择下一个点；Esc 取消");
    return;
  }
  Object o = make_object(pick_kind_);
  o.p[0] = picks_[0].x;
  o.p[1] = picks_[0].y;
  if (pick_kind_ == Kind::Circle2)
    o.p[2] = norm(subtract(picks_[1], picks_[0]));
  else if (required > 1) {
    Vec3 b = pick_kind_ == Kind::Vector2 ? subtract(picks_[1], picks_[0])
                                         : picks_[1];
    o.p[2] = b.x;
    o.p[3] = b.y;
    if (required == 3) {
      o.p[4] = p.x;
      o.p[5] = p.y;
    }
  }
  Text error;
  pick_count_ = 0;
  if (!valid_object(o, error)) {
    update_status(error.c_str());
    return;
  }
  if (!checkpoint())
    return;
  o.color = int(document.objects.size() % 8);
  if (!append(document, o, active_)) {
    update_status("不能添加对象，可能已达到 64 个对象上限");
    return;
  }
  pick_kind_ = Kind::Count;
  changed();
  refresh();
}
bool MathWindow::finish_dirty() {
  if (!apply_fields())
    return false;
  if (!dirty_)
    return true;
  int answer = fl_choice("几何文档尚未保存。", "取消", "保存", "放弃更改");
  if (answer == 2)
    return true;
  if (answer == 1)
    file_action(Command::Save);
  return false;
}
void MathWindow::run(Command command) {
  if (io_.busy())
    return;
  if (command == Command::Apply) {
    apply_fields();
    return;
  }
  if (command == Command::Close) {
    closing_ = true;
    if (finish_dirty())
      hide();
    else if (!io_.busy())
      closing_ = false;
    return;
  }
  if (command == Command::Save || command == Command::SaveAs ||
      command == Command::Open) {
    if (command == Command::Open && !finish_dirty())
      return;
    file_action(command);
    return;
  }
  if (command == Command::New || command == Command::Example2 ||
      command == Command::Example3 || command == Command::ExampleTorus ||
      command == Command::ExamplePolar) {
    if (!finish_dirty())
      return;
    history_.clear();
    if (command == Command::New)
      document = Document{};
    else
      example(document, command == Command::Example2       ? 0
                        : command == Command::Example3     ? 1
                        : command == Command::ExampleTorus ? 2
                                                           : 3);
    path_.clear();
    stamp_ = rt::FileStamp{};
    active_ = document.objects.empty() ? 0 : document.objects[0].id;
    dirty_ = false;
    refill();
    refresh();
    select_tab(document.view.tab);
    for (int n = 0; n < 4; ++n) {
      parameter_[n]->value("1");
      sliders_[n]->value(1);
    }
    resample();
    return;
  }
  if (command == Command::Undo || command == Command::Redo) {
    if (fields_dirty_) {
      fields_dirty_ = false;
      refresh();
    }
    bool ok = command == Command::Undo ? history_.undo(document)
                                       : history_.redo(document);
    if (ok) {
      active_ = document.objects.empty() ? 0 : document.objects[0].id;
      changed();
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
    }
    return;
  }
  if (command == Command::Help) {
    help_dialog();
    return;
  }
  if (!apply_fields())
    return;
  Object *o = current();
  View &v = document.view;
  switch (command) {
  case Command::Duplicate:
    if (o && checkpoint()) {
      Object copy = *o;
      if (!append(document, copy, active_)) {
        fl_alert("不能复制对象，文档最多 64 个对象");
        break;
      }
      changed();
      refresh();
    }
    break;
  case Command::Delete:
    if (o && checkpoint()) {
      for (size_t n = 0; n < document.objects.size(); ++n)
        if (document.objects[n].id == active_) {
          document.objects.erase(n);
          break;
        }
      active_ = document.objects.empty() ? 0 : document.objects[0].id;
      changed();
      refresh();
    }
    break;
  case Command::Toggle:
    if (o && checkpoint()) {
      o->visible = !o->visible;
      changed(false);
      refresh();
    }
    break;
  case Command::Reset:
    v.cx = v.cy = v.tx = v.ty = v.tz = 0;
    v.span = 14;
    v.yaw = .65;
    v.pitch = .55;
    v.scale3 = 1;
    view_changed();
    break;
  case Command::ZoomIn:
  case Command::ZoomOut:
    if (tab())
      v.scale3 = fmax(
          .01,
          fmin(100, v.scale3 * (command == Command::ZoomIn ? 1.3 : 1 / 1.3)));
    else
      v.span = fmax(
          1e-5,
          fmin(1e6, v.span * (command == Command::ZoomIn ? 1 / 1.3 : 1.3)));
    view_changed();
    break;
  case Command::Grid:
    v.grid = !v.grid;
    grid_->value(v.grid);
    view_changed();
    break;
  case Command::Axes:
    v.axes = !v.axes;
    axes_->value(v.axes);
    view_changed();
    break;
  case Command::Wire:
    v.wire = !v.wire;
    wire_->value(v.wire);
    view_changed();
    break;
  case Command::Trace:
    trace_mode_ = !trace_mode_;
    trace_->value(trace_mode_);
    break;
  case Command::Fit: {
    double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY,
           zmin = INFINITY, zmax = -INFINITY;
    auto include = [&](Vec3 p) {
      if (!finite(p) || norm(p) > 1e6)
        return;
      xmin = fmin(xmin, p.x);
      xmax = fmax(xmax, p.x);
      ymin = fmin(ymin, p.y);
      ymax = fmax(ymax, p.y);
      zmin = fmin(zmin, p.z);
      zmax = fmax(zmax, p.z);
    };
    for (size_t n = 0; n < meshes.size(); ++n)
      if (document.objects[n].visible &&
          info(document.objects[n].kind).dim3 == bool(tab())) {
        const Mesh &m = meshes[n];
        for (size_t i = 0; i < m.lines.size(); ++i) {
          include(m.lines[i].a);
          include(m.lines[i].b);
        }
        for (size_t i = 0; i < m.points.size(); ++i)
          include(m.points[i]);
      }
    if (isfinite(xmin)) {
      if (tab()) {
        v.tx = (xmin + xmax) / 2;
        v.ty = (ymin + ymax) / 2;
        v.tz = (zmin + zmax) / 2;
        double radius = norm({xmax - xmin, ymax - ymin, zmax - zmin}) / 2;
        v.scale3 = fmax(.01, fmin(100, 5 / fmax(.1, radius)));
      } else {
        v.cx = (xmin + xmax) / 2;
        v.cy = (ymin + ymax) / 2;
        v.span = fmax(.01, fmin(1e6, 1.2 * fmax(xmax - xmin,
                                                (ymax - ymin) * canvas[0]->w() /
                                                    canvas[0]->h())));
      }
      view_changed();
    }
    break;
  }
  case Command::Surface:
    surface_dialog();
    break;
  case Command::Calculate:
    calculate_dialog();
    break;
  case Command::Table:
    table_dialog();
    break;
  case Command::Vectors:
    vectors_dialog();
    break;
  case Command::Measure:
    if (o) {
      Measures m = measure(*o);
      if (m.any)
        fl_message("%s\n长度 / 周长 ≈ %.12g\n面积 / 表面积 ≈ %.12g\n体积 ≈ "
                   "%.12g\n（不适用项为 0；椭圆周长为近似值）",
                   o->name.c_str(), m.length, m.area, m.volume);
      else
        fl_message("此对象无内置几何度量。函数对象请使用“函数计算”。");
    }
    break;
  case Command::ExportSvg:
  case Command::ExportPng:
    export_action(command);
    break;
  case Command::PickPoint:
  case Command::PickSegment:
  case Command::PickCircle:
  case Command::PickTriangle:
  case Command::PickVector:
    pick_kind_ = command == Command::PickPoint     ? Kind::Point2
                 : command == Command::PickSegment ? Kind::Segment2
                 : command == Command::PickCircle  ? Kind::Circle2
                 : command == Command::PickVector  ? Kind::Vector2
                                                   : Kind::Triangle2;
    pick_count_ = 0;
    select_tab(0);
    update_status("在二维画布上选择点；Esc 取消拾点");
    break;
  case Command::CancelPick:
    pick_kind_ = Kind::Count;
    pick_count_ = 0;
    update_status();
    break;
  default:
    break;
  }
}
void MathWindow::tick(void *p) {
  auto *s = static_cast<MathWindow *>(p);
  s->poll_io();
  s->poll_sampling();
  if (s->shown())
    Fl::repeat_timeout(.03, tick, p);
}
int MathWindow::handle(int e) {
  if ((e == FL_SHORTCUT || e == FL_KEYDOWN) && (Fl::event_state() & FL_CTRL)) {
    int key = Fl::event_key();
    if (key == FL_Enter || key == FL_KP_Enter) {
      run(Command::Apply);
      return 1;
    }
    if (key == 's') {
      run(Fl::event_state() & FL_SHIFT ? Command::SaveAs : Command::Save);
      return 1;
    }
    if (key == 'o') {
      run(Command::Open);
      return 1;
    }
    if (key == 'n') {
      run(Command::New);
      return 1;
    }
    if (key == 'd') {
      run(Command::Duplicate);
      return 1;
    }
    if (key == 'z' && !fields_dirty_) {
      run(Command::Undo);
      return 1;
    }
    if (key == 'y' && !fields_dirty_) {
      run(Command::Redo);
      return 1;
    }
  }
  return Fl_Double_Window::handle(e);
}
} // namespace rmp
