#include "canvas.h"
#include "window.h"
#include <math.h>
#include <stdio.h>
namespace rd {
namespace {
bool clone(const Document &d, Document &next) {
  if (next.clone_from(d))
    return true;
  fl_alert("内存不足，无法编辑文档");
  return false;
}
} // namespace
void DiagramWindow::add_shape(Shape shape) {
  if (busy() || !apply_fields())
    return;
  Document next;
  if (!clone(document, next))
    return;
  double x = document.view.cx - 80, y = document.view.cy - 32;
  if (document.view.snap) {
    x = round(x / 20) * 20;
    y = round(y / 20) * 20;
  }
  uint64_t id;
  Text e;
  if (!add_node(next, shape, shape_name(shape), x, y, 0, id, e)) {
    fl_alert("%s", e.c_str());
    return;
  }
  if (shape == Shape::Diamond) {
    node(next, id)->w = 180;
    node(next, id)->h = 100;
  }
  if (shape == Shape::Note) {
    node(next, id)->style.fill = 0xfff5ce;
    node(next, id)->style.line = 0xb89b52;
  }
  if (commit(static_cast<Document &&>(next))) {
    select(id);
    canvas->take_focus();
    update_status("已添加图形；双击文字或在右侧编辑属性");
  }
}
void DiagramWindow::connect(uint64_t from, uint64_t to) {
  if (!apply_fields())
    return;
  Document next;
  if (!clone(document, next))
    return;
  uint64_t id;
  Text e;
  if (!add_edge(next, from, to, Route::Orthogonal, id, e)) {
    update_status(e.c_str());
    return;
  }
  if (commit(static_cast<Document &&>(next))) {
    select(id);
    update_status("已创建连线；可在右侧设置标签、线型和箭头");
  }
}
void DiagramWindow::edit_label() {
  if (selection.empty() || !apply_fields())
    return;
  const Node *n = node(document, selection[0]);
  const Edge *a = edge(document, selection[0]);
  const char *s =
      fl_input("编辑文字（多行文字请使用右侧属性）", n   ? n->text.c_str()
                                                     : a ? a->text.c_str()
                                                         : "");
  if (!s)
    return;
  Document next;
  if (!clone(document, next))
    return;
  Text *text =
      n ? &node(next, selection[0])->text : &edge(next, selection[0])->text;
  if (!text->set(s)) {
    fl_alert("内存不足");
    return;
  }
  commit(static_cast<Document &&>(next));
  canvas->take_focus();
}
void DiagramWindow::nudge(double x, double y) {
  if (selection.empty() || !apply_fields())
    return;
  Document next;
  if (!clone(document, next))
    return;
  bool any = false;
  TreeIndex tree(next);
  for (size_t i = 0; i < next.nodes.size(); ++i) {
    Node &n = next.nodes[i];
    bool move = selected(n.id);
    for (size_t j = 0; !move && j < selection.size(); ++j)
      move = tree.descendant(i, selection[j]);
    if (move) {
      n.x = fmax(-100000, fmin(100000, n.x + x));
      n.y = fmax(-100000, fmin(100000, n.y + y));
      any = true;
    }
  }
  if (any)
    commit(static_cast<Document &&>(next));
}
void DiagramWindow::copy_selection() {
  if (selection.empty())
    return;
  Document copy;
  if (!clone(document, copy))
    return;
  Array<uint64_t> remove;
  TreeIndex tree(document);
  for (size_t i = 0; i < document.nodes.size(); ++i) {
    const Node &n = document.nodes[i];
    bool keep = selected(n.id);
    for (size_t j = 0; !keep && j < selection.size(); ++j)
      keep = tree.descendant(i, selection[j]);
    if (!keep && !remove.push(n.id)) {
      fl_alert("内存不足");
      return;
    }
  }
  erase(copy, remove, false);
  clipboard_ = static_cast<Document &&>(copy);
  update_status(clipboard_.nodes.empty()
                    ? "请先选择图形；连线随两端图形一起复制"
                    : "已复制图形和内部连线，可在本文档粘贴");
}
void DiagramWindow::paste_selection() {
  if (clipboard_.nodes.empty()) {
    update_status("剪贴板中没有图形");
    return;
  }
  if (document.nodes.size() + clipboard_.nodes.size() > MaxNodes ||
      document.edges.size() + clipboard_.edges.size() > MaxEdges) {
    fl_alert("粘贴后对象数量超过限制");
    return;
  }
  Document next;
  if (!clone(document, next))
    return;
  uint64_t fresh[MaxNodes]{};
  Array<uint64_t> ids;
  Text e;
  auto mapped = [&](uint64_t id) -> uint64_t {
    for (size_t i = 0; i < clipboard_.nodes.size(); ++i)
      if (clipboard_.nodes[i].id == id)
        return fresh[i];
    return 0;
  };
  for (size_t i = 0; i < clipboard_.nodes.size(); ++i) {
    const Node &a = clipboard_.nodes[i];
    if (!add_node(next, a.shape, a.text.c_str(), a.x + 24, a.y + 24, 0,
                  fresh[i], e) ||
        !ids.push(fresh[i])) {
      fl_alert("%s", e.empty() ? "内存不足" : e.c_str());
      return;
    }
    Node *n = node(next, fresh[i]);
    uint64_t id = n->id;
    *n = a;
    n->id = id;
    n->parent = 0;
    n->x += 24;
    n->y += 24;
  }
  for (size_t i = 0; i < clipboard_.nodes.size(); ++i)
    node(next, fresh[i])->parent = mapped(clipboard_.nodes[i].parent);
  for (size_t i = 0; i < clipboard_.edges.size(); ++i) {
    const Edge &a = clipboard_.edges[i];
    uint64_t id;
    if (!add_edge(next, mapped(a.from), mapped(a.to), a.route, id, e)) {
      fl_alert("%s", e.c_str());
      return;
    }
    Edge *b = edge(next, id);
    *b = a;
    b->id = id;
    b->from = mapped(a.from);
    b->to = mapped(a.to);
  }
  if (commit(static_cast<Document &&>(next))) {
    selection.swap(ids);
    selection_changed();
  }
}
bool DiagramWindow::finish_dirty() {
  if (!apply_fields())
    return false;
  if (!dirty_)
    return true;
  int answer = fl_choice("当前文档有未保存的修改。", "取消", "不保存", "保存");
  if (answer == 1)
    return true;
  if (answer == 2)
    file_action(Command::Save);
  return false;
}
void DiagramWindow::run(Command c) {
  if (busy()) {
    update_status("正在处理文件，请稍候");
    return;
  }
  bool replacing = c == Command::New || c == Command::Open ||
                   c == Command::Close || c == Command::TemplateMind ||
                   c == Command::TemplateFlow || c == Command::TemplateLogic;
  if (replacing && !finish_dirty()) {
    if (operation_ == Operation::Save) {
      resume_ = c;
      resume_pending_ = true;
    }
    return;
  }
  if (c == Command::Close) {
    hide();
    return;
  }
  if (c == Command::Open || c == Command::Save || c == Command::SaveAs) {
    file_action(c);
    return;
  }
  if (c == Command::Apply) {
    apply_fields();
    return;
  }
  if (c == Command::Undo || c == Command::Redo) {
    if (fields_dirty_) {
      refresh();
      update_status("已还原未应用的属性");
      return;
    }
    View view = document.view;
    bool ok =
        c == Command::Undo ? history_.undo(document) : history_.redo(document);
    if (ok) {
      document.view = view;
      dirty_ = true;
      changed();
    } else
      update_status("没有可用的历史记录");
    return;
  }
  if (!apply_fields())
    return;
  if (c == Command::New || c == Command::TemplateMind ||
      c == Command::TemplateFlow || c == Command::TemplateLogic) {
    Document next;
    Text e;
    if (c == Command::New) {
      next.title.set("未命名图形");
    } else if (!example(next,
                        c == Command::TemplateMind   ? 0
                        : c == Command::TemplateFlow ? 1
                                                     : 2,
                        e)) {
      fl_alert("%s", e.c_str());
      return;
    }
    document = static_cast<Document &&>(next);
    selection.clear();
    history_.clear();
    path_.clear();
    stamp_ = {};
    dirty_ = c != Command::New;
    notice_.clear();
    changed();
    canvas->fit();
    return;
  }
  if (c == Command::ExportSvg || c == Command::ExportPng ||
      c == Command::ExportOutline) {
    export_action(c);
    return;
  }
  if (c == Command::Fit) {
    canvas->fit();
    return;
  }
  if (c == Command::ZoomIn || c == Command::ZoomOut) {
    canvas->zoom(c == Command::ZoomIn ? 1.2 : 1 / 1.2);
    return;
  }
  if (c == Command::ActualSize) {
    document.view.zoom = 1;
    canvas->redraw();
    update_status();
    return;
  }
  if (c == Command::Grid || c == Command::Snap) {
    if (c == Command::Grid)
      document.view.grid = !document.view.grid;
    else
      document.view.snap = !document.view.snap;
    canvas->redraw();
    update_status();
    return;
  }
  if (c == Command::SelectMode || c == Command::ConnectMode ||
      c == Command::PanMode) {
    canvas->cancel();
    mode_->value(c == Command::ConnectMode ? 1 : c == Command::PanMode ? 2 : 0);
    choose(mode_, this);
    canvas->take_focus();
    return;
  }
  if (c == Command::SelectAll) {
    if (!canvas->prepare() ||
        !selection.clone_from(canvas->frame().visible_nodes)) {
      update_status("选择内存不足");
      return;
    }
    selection_changed();
    return;
  }
  if (c == Command::Copy) {
    copy_selection();
    return;
  }
  if (c == Command::Paste) {
    paste_selection();
    return;
  }
  if (c == Command::Duplicate) {
    copy_selection();
    paste_selection();
    return;
  }
  if (c == Command::Help) {
    fl_message(
        "ReliefDiagram\n\n图形：双击左侧图形库或使用插入工具。双击图形 / F2 "
        "编辑文字。\n主题：选中主题后 Tab 添加子主题，Enter "
        "添加同级主题；折叠保留原数据。\n连线：选择“连接图形”，依次点击起点与终"
        "点；Esc 返回选择工具。\n画布：拖动移动，右下角手柄调整尺寸；Shift "
        "多选，拖动空白框选。\n中键 / 右键或平移工具平移，滚轮缩放，Home "
        "适合内容。\n方向键微调，Shift+方向键移动 "
        "20；布局页提供对齐、分布和层级。\n右侧修改后点击应用；多选样式应用到同"
        "类对象，文字和位置仅修改首个。\nCtrl+S 保存，Ctrl+Z / Y "
        "撤销重做，Ctrl+C / V 使用应用内部图形剪贴板。\n\n.rdiagram "
        "保存全部可编辑内容；SVG / PNG 导出完整展开图形，Markdown "
        "包含折叠分支。\n首版最多 512 个图形、2048 条附加连线、8 "
        "MiB；不导入其他厂商格式。");
    return;
  }
  Document next;
  if (!clone(document, next))
    return;
  Text e;
  if (c == Command::Title) {
    const char *title = fl_input("文档标题", document.title.c_str());
    if (!title)
      return;
    if (!next.title.set(title)) {
      fl_alert("内存不足");
      return;
    }
  } else if (c == Command::MindLayout || c == Command::TreeLayout ||
             c == Command::FlowLayout) {
    if (!arrange(next,
                 c == Command::MindLayout   ? Layout::Mind
                 : c == Command::TreeLayout ? Layout::Tree
                                            : Layout::Flow,
                 e)) {
      fl_alert("%s", e.c_str());
      return;
    }
  } else if (c == Command::Delete) {
    if (selection.empty())
      return;
    erase(next, selection);
  } else if (c == Command::Child || c == Command::Sibling) {
    Node *n = selection.empty() ? nullptr : node(next, selection[0]);
    uint64_t parent = 0;
    if (n) {
      parent = c == Command::Child ? n->id : n->parent;
      if (c == Command::Child)
        n->collapsed = false;
    }
    Style inherited = n ? n->style : Style{};
    bool inherit = n != nullptr;
    uint64_t id;
    if (!add_node(next, Shape::Rounded, parent ? "新主题" : "中心主题",
                  document.view.cx, document.view.cy, parent, id, e)) {
      fl_alert("%s", e.c_str());
      return;
    }
    if (inherit)
      node(next, id)->style = inherited;
    if (!arrange(next, Layout::Mind, e)) {
      fl_alert("%s", e.c_str());
      return;
    }
    if (commit(static_cast<Document &&>(next))) {
      select(id);
      canvas->fit();
      canvas->take_focus();
    }
    return;
  } else if (c == Command::Fold) {
    if (selection.empty())
      return;
    for (size_t i = 0; i < selection.size(); ++i)
      if (Node *n = node(next, selection[i]))
        n->collapsed = !n->collapsed;
  } else if (c == Command::Front || c == Command::Back) {
    Array<Node> order;
    for (int pass = 0; pass < 2; ++pass)
      for (size_t i = 0; i < next.nodes.size(); ++i) {
        bool is_selected = selected(next.nodes[i].id),
             want = pass == (c == Command::Front ? 1 : 0);
        if (is_selected == want && !order.push(next.nodes[i])) {
          fl_alert("内存不足");
          return;
        }
      }
    next.nodes.swap(order);
  } else {
    size_t indices[MaxNodes], count = 0;
    double left = 1e30, top = 1e30, right = -1e30, bottom = -1e30;
    for (size_t i = 0; i < next.nodes.size(); ++i)
      if (selected(next.nodes[i].id)) {
        const Node &n = next.nodes[i];
        indices[count++] = i;
        left = fmin(left, n.x);
        top = fmin(top, n.y);
        right = fmax(right, n.x + n.w);
        bottom = fmax(bottom, n.y + n.h);
      }
    bool distribute = c == Command::DistributeH || c == Command::DistributeV;
    if (count < (distribute ? 3U : 2U)) {
      update_status(distribute ? "请至少选择三个图形" : "请至少选择两个图形");
      return;
    }
    if (distribute) {
      bool horizontal = c == Command::DistributeH;
      for (size_t i = 1; i < count; ++i)
        for (size_t j = i; j > 0; --j) {
          const Node &a = next.nodes[indices[j - 1]],
                     &b = next.nodes[indices[j]];
          if ((horizontal ? a.x : a.y) <= (horizontal ? b.x : b.y))
            break;
          size_t t = indices[j];
          indices[j] = indices[j - 1];
          indices[j - 1] = t;
        }
      double sizes = 0;
      for (size_t i = 0; i < count; ++i)
        sizes +=
            horizontal ? next.nodes[indices[i]].w : next.nodes[indices[i]].h;
      double start = horizontal ? left : top,
             gap = ((horizontal ? right - left : bottom - top) - sizes) /
                   (count - 1);
      for (size_t i = 0; i < count; ++i) {
        Node &n = next.nodes[indices[i]];
        if (horizontal)
          n.x = start;
        else
          n.y = start;
        start += (horizontal ? n.w : n.h) + gap;
      }
    } else
      for (size_t i = 0; i < count; ++i) {
        Node &n = next.nodes[indices[i]];
        if (c == Command::AlignLeft)
          n.x = left;
        else if (c == Command::AlignTop)
          n.y = top;
        else if (c == Command::AlignCenter)
          n.x = (left + right - n.w) / 2;
        else if (c == Command::AlignMiddle)
          n.y = (top + bottom - n.h) / 2;
      }
  }
  if (commit(static_cast<Document &&>(next)) &&
      (c == Command::MindLayout || c == Command::TreeLayout ||
       c == Command::FlowLayout))
    canvas->fit();
}
int DiagramWindow::handle(int event) {
  if ((event == FL_KEYDOWN || event == FL_SHORTCUT) &&
      Fl::event_key() == FL_Enter && Fl::event_state(FL_CTRL)) {
    if (!busy())
      apply_fields();
    return 1;
  }
  if ((event == FL_KEYDOWN || event == FL_SHORTCUT) &&
      Fl::event_key() == FL_Escape) {
    if (busy())
      return 1;
    canvas->cancel();
    mode_->value(0);
    if (fields_dirty_)
      refresh();
    canvas->take_focus();
    update_status("已返回选择工具");
    return 1;
  }
  if (Fl_Double_Window::handle(event))
    return 1;
  if (event == FL_SHORTCUT && Fl::event_state(FL_CTRL)) {
    if (Fl::event_key() == '+' || Fl::event_key() == '=') {
      run(Command::ZoomIn);
      return 1;
    }
    if (Fl::event_key() == '-') {
      run(Command::ZoomOut);
      return 1;
    }
  }
  return 0;
}
} // namespace rd
