#include "model.h"
#include <math.h>
#include <stdio.h>
namespace rd {
namespace {
bool fail(Text &e, const char *s) {
  e.set(s);
  return false;
}
bool has(const Array<uint64_t> &ids, uint64_t id) {
  for (size_t i = 0; i < ids.size(); ++i)
    if (ids[i] == id)
      return true;
  return false;
}
bool coordinate(double x) { return isfinite(x) && fabs(x) <= 100000; }
bool text_valid(const Text &s, size_t limit) {
  return s.size() <= limit && utf8(s.c_str(), s.size());
}
size_t document_bytes(const Document &d) {
  size_t bytes = 62; // RDGM v1 fixed header, document fields and counts.
  auto append = [&](size_t count) {
    if (count > MaxFileBytes - bytes)
      return false;
    bytes += count;
    return true;
  };
  if (!append(d.title.size()))
    return MaxFileBytes + 1;
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    const Node &n = d.nodes[i];
    if (!append(72) || !append(n.text.size()) || !append(n.note.size()))
      return MaxFileBytes + 1;
  }
  for (size_t i = 0; i < d.edges.size(); ++i)
    if (!append(35) || !append(d.edges[i].text.size()))
      return MaxFileBytes + 1;
  return bytes;
}
double branch_height(const Document &d, const Node &n) {
  if (n.collapsed)
    return n.h;
  double h = 0;
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].parent == n.id)
      h += branch_height(d, d.nodes[i]) + 24;
  return fmax(n.h, h ? h - 24 : 0);
}
void branch(Document &d, uint64_t id, double x, double center, int side) {
  Node *n = node(d, id);
  if (!n)
    return;
  n->x = x;
  n->y = center - n->h / 2;
  if (n->collapsed)
    return;
  double total = 0;
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].parent == id)
      total += branch_height(d, d.nodes[i]) + 24;
  double top = center - (total ? total - 24 : 0) / 2;
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].parent == id) {
      Node &c = d.nodes[i];
      double h = branch_height(d, c);
      branch(d, c.id, side > 0 ? x + n->w + 64 : x - 64 - c.w, top + h / 2,
             side);
      top += h + 24;
    }
}
} // namespace
const char *shape_name(Shape s) {
  const char *names[] = {"矩形",     "圆角矩形", "判断（菱形）", "终止（椭圆）",
                         "输入输出", "文档",     "数据库",       "便笺"};
  return unsigned(s) < unsigned(Shape::Count) ? names[unsigned(s)] : "未知图形";
}
bool Document::clone_from(const Document &d) {
  Document copy;
  if (!copy.nodes.clone_from(d.nodes) || !copy.edges.clone_from(d.edges))
    return false;
  copy.title = d.title;
  copy.next_id = d.next_id;
  copy.view = d.view;
  *this = static_cast<Document &&>(copy);
  return true;
}
Node *node(Document &d, uint64_t id) {
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].id == id)
      return &d.nodes[i];
  return nullptr;
}
const Node *node(const Document &d, uint64_t id) {
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].id == id)
      return &d.nodes[i];
  return nullptr;
}
Edge *edge(Document &d, uint64_t id) {
  for (size_t i = 0; i < d.edges.size(); ++i)
    if (d.edges[i].id == id)
      return &d.edges[i];
  return nullptr;
}
bool utf8(const char *s, size_t n) {
  for (size_t i = 0; i < n;) {
    unsigned c = static_cast<unsigned char>(s[i++]);
    if (c < 128) {
      if ((c < 32 && c != '\n' && c != '\t') || c == 127)
        return false;
      continue;
    }
    unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 99;
    if (count == 99 || count > n - i)
      return false;
    unsigned value = c & ((1U << (6 - count)) - 1);
    for (unsigned j = 0; j < count; ++j) {
      unsigned t = static_cast<unsigned char>(s[i++]);
      if ((t & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (t & 63);
    }
    if ((count == 1 && value < 128) || (count == 2 && value < 2048) ||
        (count == 3 && value < 65536) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}
TreeIndex::TreeIndex(const Document &d) : document_(d) {
  if (d.nodes.size() > MaxNodes)
    return;
  unsigned char state[MaxNodes]{};
  int path[MaxNodes];
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    parents_[i] = d.nodes[i].parent ? index(d.nodes[i].parent) : -1;
    if (d.nodes[i].parent && parents_[i] < 0)
      return;
  }
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    int at = int(i), depth = 0;
    while (at >= 0 && !state[at]) {
      path[depth++] = at;
      state[at] = 1;
      at = parents_[at];
    }
    if (at >= 0 && state[at] == 1)
      return;
    while (depth) {
      int child = path[--depth], parent = parents_[child];
      shown_[child] =
          parent < 0 || (shown_[parent] && !d.nodes[size_t(parent)].collapsed);
      state[child] = 2;
    }
  }
  valid_ = true;
}
int TreeIndex::index(uint64_t id) const {
  for (size_t i = 0; i < document_.nodes.size(); ++i)
    if (document_.nodes[i].id == id)
      return int(i);
  return -1;
}
bool TreeIndex::descendant(size_t i, uint64_t ancestor) const {
  if (!valid_ || i >= document_.nodes.size())
    return false;
  int target = index(ancestor), at = parents_[i];
  if (target < 0)
    return false;
  while (at >= 0) {
    if (at == target)
      return true;
    at = parents_[at];
  }
  return false;
}
bool descendant(const Document &d, uint64_t id, uint64_t ancestor) {
  TreeIndex tree(d);
  int at = tree.index(id);
  return at >= 0 && tree.descendant(size_t(at), ancestor);
}
bool visible(const Document &d, const Node &n) {
  TreeIndex tree(d);
  int at = tree.index(n.id);
  return at >= 0 && tree.shown(size_t(at));
}
bool validate(const Document &d, Text &e) {
  if (d.nodes.size() > MaxNodes || d.edges.size() > MaxEdges ||
      !text_valid(d.title, 512) || !d.next_id || d.next_id == UINT64_MAX)
    return fail(e, "文档超过限制或标题无效");
  if (!coordinate(d.view.cx) || !coordinate(d.view.cy) ||
      !isfinite(d.view.zoom) || d.view.zoom < .15 || d.view.zoom > 4)
    return fail(e, "视图参数无效");
  if (document_bytes(d) > MaxFileBytes)
    return fail(e, "文档总大小超过 8 MiB，未应用修改");
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    const Node &n = d.nodes[i];
    if (!n.id || n.id >= d.next_id ||
        unsigned(n.shape) >= unsigned(Shape::Count) || !coordinate(n.x) ||
        !coordinate(n.y) || !isfinite(n.w) || !isfinite(n.h) || n.w < 40 ||
        n.h < 28 || n.w > 2000 || n.h > 2000 || n.style.font < 10 ||
        n.style.font > 48 ||
        (n.style.fill | n.style.line | n.style.text) > 0xffffff ||
        !text_valid(n.text, 4096) || !text_valid(n.note, 16384))
      return fail(e, "图形的编号、文字、尺寸或样式无效");
    for (size_t j = 0; j < i; ++j)
      if (d.nodes[j].id == n.id)
        return fail(e, "图形编号重复");
  }
  TreeIndex tree(d);
  if (!tree.valid())
    return fail(e, "主题父子关系缺失或存在循环");
  for (size_t i = 0; i < d.edges.size(); ++i) {
    const Edge &a = d.edges[i];
    if (!a.id || a.id >= d.next_id || node(d, a.id) || !node(d, a.from) ||
        !node(d, a.to) || a.from == a.to ||
        unsigned(a.route) >= unsigned(Route::Count) || a.arrows > 2 ||
        a.width < 1 || a.width > 6 || a.color > 0xffffff ||
        !text_valid(a.text, 1024))
      return fail(e, "连线的编号、端点或样式无效");
    for (size_t j = 0; j < i; ++j)
      if (d.edges[j].id == a.id)
        return fail(e, "连线编号重复");
  }
  e.clear();
  return true;
}
bool add_node(Document &d, Shape s, const char *text, double x, double y,
              uint64_t parent, uint64_t &id, Text &e) {
  if (d.nodes.size() >= MaxNodes || d.next_id >= UINT64_MAX - 1)
    return fail(e, "最多支持 512 个图形");
  if (parent && !node(d, parent))
    return fail(e, "父主题不存在");
  if (!text || strlen(text) > 4096 || !utf8(text, strlen(text)) ||
      !coordinate(x) || !coordinate(y) || unsigned(s) >= unsigned(Shape::Count))
    return fail(e, "图形参数无效");
  size_t bytes = document_bytes(d);
  if (bytes > MaxFileBytes || 72 + strlen(text) > MaxFileBytes - bytes)
    return fail(e, "添加图形后文档将超过 8 MiB");
  Node n;
  n.id = d.next_id;
  n.parent = parent;
  n.shape = s;
  n.x = x;
  n.y = y;
  if (!n.text.set(text) || !d.nodes.push(n))
    return fail(e, "内存不足");
  id = d.next_id++;
  e.clear();
  return true;
}
bool add_edge(Document &d, uint64_t from, uint64_t to, Route r, uint64_t &id,
              Text &e) {
  if (!node(d, from) || !node(d, to) || from == to ||
      unsigned(r) >= unsigned(Route::Count))
    return fail(e, "请选择两个不同的图形");
  if (d.edges.size() >= MaxEdges || d.next_id >= UINT64_MAX - 1)
    return fail(e, "最多支持 2048 条连线");
  size_t bytes = document_bytes(d);
  if (bytes > MaxFileBytes || 35 > MaxFileBytes - bytes)
    return fail(e, "添加连线后文档将超过 8 MiB");
  for (size_t i = 0; i < d.edges.size(); ++i)
    if (d.edges[i].from == from && d.edges[i].to == to)
      return fail(e, "这两个图形之间已有同方向连线");
  Edge a;
  a.id = d.next_id;
  a.from = from;
  a.to = to;
  a.route = r;
  if (!d.edges.push(a))
    return fail(e, "内存不足");
  id = d.next_id++;
  e.clear();
  return true;
}
void erase(Document &d, const Array<uint64_t> &ids, bool subtree) {
  TreeIndex tree(d);
  bool removed[MaxNodes]{};
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    removed[i] = has(ids, d.nodes[i].id);
    if (subtree)
      for (size_t j = 0; !removed[i] && j < ids.size(); ++j)
        removed[i] = tree.descendant(i, ids[j]);
  }
  for (size_t i = d.nodes.size(); i > 0; --i)
    if (removed[i - 1])
      d.nodes.erase(i - 1);
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].parent && !node(d, d.nodes[i].parent))
      d.nodes[i].parent = 0;
  for (size_t i = d.edges.size(); i > 0; --i)
    if (has(ids, d.edges[i - 1].id) || !node(d, d.edges[i - 1].from) ||
        !node(d, d.edges[i - 1].to))
      d.edges.erase(i - 1);
}
bool duplicate(Document &d, const Array<uint64_t> &ids, Array<uint64_t> &result,
               Text &e) {
  Document copy;
  Array<uint64_t> old, fresh;
  if (!copy.clone_from(d))
    return fail(e, "内存不足");
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (has(ids, d.nodes[i].id)) {
      if (copy.nodes.size() >= MaxNodes || copy.next_id >= UINT64_MAX - 1)
        return fail(e, "复制后图形数量超过限制");
      Node n = d.nodes[i];
      n.id = copy.next_id++;
      n.x += 24;
      n.y += 24;
      if (!old.push(d.nodes[i].id) || !fresh.push(n.id) || !copy.nodes.push(n))
        return fail(e, "内存不足");
    }
  auto mapped = [&](uint64_t id) -> uint64_t {
    for (size_t i = 0; i < old.size(); ++i)
      if (old[i] == id)
        return fresh[i];
    return 0;
  };
  for (size_t i = d.nodes.size(); i < copy.nodes.size(); ++i)
    copy.nodes[i].parent = mapped(copy.nodes[i].parent);
  for (size_t i = 0; i < d.edges.size(); ++i) {
    const Edge &a = d.edges[i];
    if (!mapped(a.from) || !mapped(a.to))
      continue;
    if (copy.edges.size() >= MaxEdges || copy.next_id >= UINT64_MAX - 1)
      return fail(e, "复制后连线数量超过限制");
    Edge b = a;
    b.id = copy.next_id++;
    b.from = mapped(a.from);
    b.to = mapped(a.to);
    if (!copy.edges.push(b))
      return fail(e, "内存不足");
  }
  if (!validate(copy, e))
    return false;
  d = static_cast<Document &&>(copy);
  result.swap(fresh);
  return true;
}
static bool arrange_in_place(Document &d, Layout kind, Text &e) {
  if (!validate(d, e))
    return false;
  if (kind == Layout::Flow) {
    int rank[MaxNodes], rows[MaxNodes]{};
    size_t queue[MaxNodes], head = 0, tail = 0;
    for (size_t i = 0; i < d.nodes.size(); ++i)
      rank[i] = -1;
    auto index = [&](uint64_t id) -> size_t {
      for (size_t i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].id == id)
          return i;
      return d.nodes.size();
    };
    for (size_t i = 0; i < d.nodes.size(); ++i) {
      bool incoming = d.nodes[i].parent != 0;
      for (size_t j = 0; j < d.edges.size(); ++j)
        if (d.edges[j].to == d.nodes[i].id)
          incoming = true;
      if (!incoming) {
        rank[i] = 0;
        queue[tail++] = i;
      }
    }
    for (size_t seed = 0; seed < d.nodes.size(); ++seed) {
      if (head == tail && rank[seed] < 0) {
        rank[seed] = 0;
        queue[tail++] = seed;
      }
      while (head < tail) {
        size_t at = queue[head++];
        auto visit = [&](uint64_t id) {
          size_t i = index(id);
          if (i < d.nodes.size() && rank[i] < 0) {
            rank[i] = rank[at] + 1;
            queue[tail++] = i;
          }
        };
        for (size_t j = 0; j < d.edges.size(); ++j)
          if (d.edges[j].from == d.nodes[at].id)
            visit(d.edges[j].to);
        for (size_t j = 0; j < d.nodes.size(); ++j)
          if (d.nodes[j].parent == d.nodes[at].id)
            visit(d.nodes[j].id);
      }
    }
    double widths[MaxNodes]{}, heights[MaxNodes]{};
    for (size_t i = 0; i < d.nodes.size(); ++i)
      widths[rank[i]] = fmax(widths[rank[i]], d.nodes[i].w);
    for (size_t i = 1; i < d.nodes.size(); ++i)
      widths[i] += widths[i - 1] + 80;
    for (size_t i = 0; i < d.nodes.size(); ++i) {
      Node &n = d.nodes[i];
      int r = rank[i];
      n.x = r ? widths[r - 1] + 80 : 0;
      n.y = heights[r];
      heights[r] += n.h + 36;
      ++rows[r];
    }
  } else {
    double root_top = 0;
    for (size_t i = 0; i < d.nodes.size(); ++i)
      if (!d.nodes[i].parent) {
        uint64_t root = d.nodes[i].id;
        Node &r = d.nodes[i];
        if (kind == Layout::Tree) {
          double h = branch_height(d, r);
          branch(d, root, 0, root_top + h / 2, 1);
          root_top += h + 80;
          continue;
        }
        double heights[2]{};
        int count = 0;
        if (!r.collapsed)
          for (size_t j = 0; j < d.nodes.size(); ++j)
            if (d.nodes[j].parent == root) {
              heights[count++ % 2] += branch_height(d, d.nodes[j]) + 24;
            }
        for (double &h : heights)
          if (h)
            h -= 24;
        double band = fmax(r.h, fmax(heights[0], heights[1])),
               center = root_top + band / 2;
        r.x = 0;
        r.y = center - r.h / 2;
        double top[] = {center - heights[0] / 2, center - heights[1] / 2};
        count = 0;
        if (!r.collapsed)
          for (size_t j = 0; j < d.nodes.size(); ++j)
            if (d.nodes[j].parent == root) {
              Node &c = d.nodes[j];
              int side = count++ % 2;
              double h = branch_height(d, c);
              branch(d, c.id, side ? -64 - c.w : r.w + 64, top[side] + h / 2,
                     side ? -1 : 1);
              top[side] += h + 24;
            }
        root_top += band + 100;
      }
  }
  return validate(d, e);
}
bool arrange(Document &d, Layout kind, Text &e) {
  Document next;
  if (!next.clone_from(d))
    return fail(e, "内存不足，无法布局");
  if (!arrange_in_place(next, kind, e))
    return false;
  d = static_cast<Document &&>(next);
  return true;
}
bool History::checkpoint(const Document &d) {
  Document copy;
  if (!copy.clone_from(d) || !past_.push(static_cast<Document &&>(copy)))
    return false;
  if (past_.size() > 32)
    past_.erase(0);
  future_.clear();
  return true;
}
bool History::transfer(Array<Document> &from, Array<Document> &to,
                       Document &d) {
  if (from.empty())
    return false;
  Document copy;
  if (!copy.clone_from(d) || !to.push(static_cast<Document &&>(copy)))
    return false;
  d = static_cast<Document &&>(from[from.size() - 1]);
  from.erase(from.size() - 1);
  return true;
}
bool History::undo(Document &d) { return transfer(past_, future_, d); }
bool History::redo(Document &d) { return transfer(future_, past_, d); }
bool example(Document &d, int kind, Text &e) {
  Document next;
  uint64_t ids[16]{};
  auto add = [&](int at, Shape s, const char *name, double x, double y,
                 uint64_t parent = 0) {
    return add_node(next, s, name, x, y, parent, ids[at], e);
  };
  if (kind == 0) {
    if (!next.title.set("项目规划") ||
        !add(0, Shape::Rounded, "项目规划", 0, 0))
      return false;
    const char *topics[] = {"目标与范围", "设计方案", "执行计划", "验收交付"};
    const char *leaves[] = {"明确需求",   "确定优先级", "界面与交互",
                            "技术与数据", "任务分工",   "里程碑",
                            "功能测试",   "文档与发布"};
    const uint32_t fills[] = {0xe8f0fb, 0xe9f5eb, 0xfff2dc, 0xf1eafa},
                   lines[] = {0x527bb2, 0x4e9268, 0xb88a38, 0x8e6db0};
    next.nodes[0].style.bold = true;
    next.nodes[0].style.font = 20;
    next.nodes[0].h = 80;
    next.nodes[0].w = 180;
    for (int i = 0; i < 4; ++i) {
      if (!add(1 + i, Shape::Rounded, topics[i], 0, 0, ids[0]))
        return false;
      next.nodes[next.nodes.size() - 1].style.fill = fills[i];
      next.nodes[next.nodes.size() - 1].style.line = lines[i];
      next.nodes[next.nodes.size() - 1].style.bold = true;
      for (int j = 0; j < 2; ++j) {
        uint64_t id;
        if (!add_node(next, Shape::Rounded, leaves[i * 2 + j], 0, 0, ids[1 + i],
                      id, e))
          return false;
        next.nodes[next.nodes.size() - 1].style.fill = fills[i];
        next.nodes[next.nodes.size() - 1].style.line = lines[i];
      }
    }
    if (!arrange(next, Layout::Mind, e))
      return false;
  } else if (kind == 1) {
    if (!next.title.set("审批流程") ||
        !add(0, Shape::Ellipse, "开始", 20, 20) ||
        !add(1, Shape::Parallelogram, "提交申请", 20, 110) ||
        !add(2, Shape::Diamond, "材料齐全？", 20, 210) ||
        !add(3, Shape::Rounded, "审批与处理", 300, 210) ||
        !add(4, Shape::Document, "补充材料", 20, 350) ||
        !add(5, Shape::Database, "归档记录", 300, 350) ||
        !add(6, Shape::Ellipse, "完成", 300, 450))
      return false;
    next.nodes[2].w = 180;
    next.nodes[2].h = 100;
    int pairs[][2] = {{0, 1}, {1, 2}, {2, 3}, {2, 4}, {4, 1}, {3, 5}, {5, 6}};
    const char *labels[] = {"", "", "是", "否", "重新提交", "", ""};
    for (int i = 0; i < 7; ++i) {
      uint64_t id;
      if (!add_edge(next, ids[pairs[i][0]], ids[pairs[i][1]], Route::Orthogonal,
                    id, e) ||
          !edge(next, id)->text.set(labels[i]))
        return false;
    }
  } else {
    if (!next.title.set("系统逻辑关系") ||
        !add(0, Shape::Rounded, "用户界面", 0, 0) ||
        !add(1, Shape::Rectangle, "业务逻辑", 260, 0) ||
        !add(2, Shape::Database, "数据存储", 520, 0) ||
        !add(3, Shape::Rectangle, "验证与权限", 260, 150) ||
        !add(4, Shape::Note, "日志 / 通知", 520, 150))
      return false;
    int pairs[][2] = {{0, 1}, {1, 2}, {1, 3}, {3, 1}, {1, 4}};
    const char *labels[] = {"操作请求", "读写", "检查", "结果", "事件"};
    for (int i = 0; i < 5; ++i) {
      uint64_t id;
      if (!add_edge(next, ids[pairs[i][0]], ids[pairs[i][1]],
                    i == 3 ? Route::Curve : Route::Orthogonal, id, e) ||
          !edge(next, id)->text.set(labels[i]))
        return false;
    }
  }
  if (!validate(next, e))
    return false;
  d = static_cast<Document &&>(next);
  return true;
}
} // namespace rd
