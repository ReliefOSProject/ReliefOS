#include "../../userland/apps/reliefdiagram/document.h"
#include "../../userland/apps/reliefdiagram/scene.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
using namespace rd;
static uint64_t put(Document &d, const char *s, uint64_t parent = 0) {
  uint64_t id = 0;
  Text e;
  assert(add_node(d, Shape::Rounded, s, 0, 0, parent, id, e));
  return id;
}
static void structure() {
  Document d;
  Text e;
  auto root = put(d, "中心主题"), a = put(d, "设计", root),
       b = put(d, "交付", root), leaf = put(d, "界面", a);
  assert(validate(d, e));
  assert(descendant(d, leaf, root));
  assert(!descendant(d, b, a));
  assert(arrange(d, Layout::Mind, e));
  const Node *r = node(d, root), *na = node(d, a), *nb = node(d, b);
  assert(na->x + na->w < r->x || na->x > r->x + r->w);
  assert(nb->x + nb->w < r->x || nb->x > r->x + r->w);
  for (size_t i = 0; i < d.nodes.size(); ++i)
    for (size_t j = i + 1; j < d.nodes.size(); ++j) {
      const Node &x = d.nodes[i], &y = d.nodes[j];
      assert(x.x + x.w <= y.x || y.x + y.w <= x.x || x.y + x.h <= y.y ||
             y.y + y.h <= x.y);
    }
  node(d, a)->collapsed = true;
  assert(!visible(d, *node(d, leaf)));
  assert(visible(d, *node(d, a)));
  assert(d.nodes.size() == 4);
  node(d, a)->collapsed = false;
  assert(visible(d, *node(d, leaf)));
  node(d, root)->parent = leaf;
  assert(!validate(d, e));
  node(d, root)->parent = 0;
  uint64_t link;
  assert(add_edge(d, leaf, b, Route::Curve, link, e));
  assert(!add_edge(d, leaf, leaf, Route::Straight, link, e));
  Array<uint64_t> ids;
  assert(ids.push(a));
  erase(d, ids);
  assert(d.nodes.size() == 2 && d.edges.empty());
  assert(!node(d, leaf));
  assert(validate(d, e));
}
static void history_and_copy() {
  Document d;
  Text e;
  auto a = put(d, "原文"), b = put(d, "子主题", a);
  uint64_t k;
  assert(add_edge(d, a, b, Route::Straight, k, e));
  History h;
  assert(h.checkpoint(d));
  assert(node(d, a)->text.set("修改"));
  assert(h.undo(d));
  assert(!strcmp(node(d, a)->text.c_str(), "原文"));
  assert(h.redo(d));
  assert(!strcmp(node(d, a)->text.c_str(), "修改"));
  Array<uint64_t> ids, out;
  assert(ids.push(a));
  assert(ids.push(b));
  assert(duplicate(d, ids, out, e));
  assert(out.size() == 2 && d.nodes.size() == 4 && d.edges.size() == 2);
  assert(node(d, out[1])->parent == out[0]);
  assert(d.edges[1].from == out[0] && d.edges[1].to == out[1]);
  assert(validate(d, e));
}
static void files() {
  Document d;
  Text e;
  assert(d.title.set("项目 <A> & B"));
  auto a = put(d, "计划 & <设计>"), b = put(d, "第二步\n多行", a);
  uint64_t k;
  assert(add_edge(d, a, b, Route::Curve, k, e));
  assert(edge(d, k)->text.set("通过"));
  assert(node(d, b)->note.set("备注：UTF-8"));
  node(d, b)->collapsed = true;
  Bytes bytes;
  assert(encode(d, bytes, e));
  assert(!memcmp(bytes.data(), "RDGM", 4));
  Document read;
  assert(decode(bytes.data(), bytes.size(), read, e));
  assert(read.nodes.size() == 2 && read.edges.size() == 1 &&
         read.nodes[1].parent == a);
  assert(read.nodes[1].note == node(d, b)->note && read.nodes[1].collapsed);
  assert(read.title == d.title && read.edges[0].text == d.edges[0].text);
  for (size_t n = 0; n < bytes.size(); ++n) {
    Document bad;
    assert(!decode(bytes.data(), n, bad, e));
  }
  bytes[bytes.size() - 1] ^= 1;
  assert(!decode(bytes.data(), bytes.size(), read, e));
  assert(read.nodes.size() == 2);
  node(d, a)->x = NAN;
  assert(!encode(d, bytes, e));
  node(d, a)->x = 0;
  assert(node(d, a)->text.set("\xc0\xaf"));
  assert(!encode(d, bytes, e));
  assert(!utf8("\xed\xa0\x80", 3));
  assert(!utf8("\xf4\x90\x80\x80", 4));
  assert(utf8("中文", 6));
}
static bool contains(const Bytes &b, const char *s) {
  size_t n = strlen(s);
  for (size_t i = 0; i + n <= b.size(); ++i)
    if (!memcmp(b.data() + i, s, n))
      return true;
  return false;
}
static void capacity_and_layout_failure() {
  Document d;
  Text e;
  uint64_t parent = 0;
  for (int i = 0; i < 448; ++i)
    parent = put(d, "主题", parent);
  assert(validate(d, e));
  assert(!arrange(d, Layout::Tree, e));
  assert(validate(d, e));
  for (size_t i = 0; i < d.nodes.size(); ++i)
    assert(d.nodes[i].x == 0 && d.nodes[i].y == 0);
  Document big;
  char note[16385];
  memset(note, 'x', 16384);
  note[16384] = 0;
  for (size_t i = 0; i < MaxNodes; ++i)
    put(big, "节点");
  for (size_t i = 0; i < MaxNodes; ++i)
    assert(big.nodes[i].note.set(note));
  assert(!validate(big, e));
  Document limit;
  char label[4097];
  memset(label, 'x', 4096);
  label[4096] = 0;
  for (int i = 0; i < 408; ++i) {
    auto id = put(limit, label);
    assert(node(limit, id)->note.set(note));
  }
  assert(validate(limit, e));
  uint64_t id = 0, sequence = limit.next_id;
  assert(!add_node(limit, Shape::Rounded, label, 0, 0, 0, id, e));
  assert(limit.nodes.size() == 408 && limit.next_id == sequence &&
         validate(limit, e));
  label[3258] = 0;
  assert(add_node(limit, Shape::Rounded, label, 0, 0, 0, id, e));
  Bytes bytes;
  assert(encode(limit, bytes, e));
  assert(bytes.size() == MaxFileBytes);
  sequence = limit.next_id;
  assert(!add_edge(limit, limit.nodes[0].id, limit.nodes[1].id, Route::Straight,
                   id, e));
  assert(limit.edges.empty() && limit.next_id == sequence &&
         validate(limit, e));
}
static void geometry_and_exports() {
  Node a, b;
  a.x = 0;
  a.y = 0;
  a.w = 100;
  a.h = 60;
  a.shape = Shape::Rectangle;
  b = a;
  b.x = 300;
  Point p = anchor(a, {350, 30});
  assert(fabs(p.x - 100) < .001 && fabs(p.y - 30) < .001);
  Stroke c = connection(a, b, Route::Straight);
  assert(c.count == 2);
  assert(fabs(c.points[0].x - 100) < .001 && fabs(c.points[1].x - 300) < .001);
  assert(inside(a, {1, 1}) && !inside(a, {-1, 1}));
  a.shape = Shape::Diamond;
  assert(!inside(a, {1, 1}) && inside(a, {50, 30}));
  assert(distance(c, {200, 30}) < .001 && distance(c, {200, 50}) > 19);
  a.shape = Shape::Rectangle;
  a.y = 300;
  b.x = 0;
  b.y = 0;
  c = connection(a, b, Route::Orthogonal);
  assert(c.count == 4 && c.points[1].x < 0 && c.points[2].x < 0);
  Document d;
  Text e;
  auto root = put(d, "<起点> & \"引号\""), child = put(d, "被折叠内容", root);
  node(d, root)->collapsed = true;
  Frame f;
  assert(build_frame(d, f, e));
  Bytes out;
  assert(svg(f, out, e));
  assert(contains(out, "&lt;起点&gt; &amp; &quot;引号&quot;"));
  assert(!contains(out, "被折叠内容"));
  assert(markdown(d, out, e));
  assert(contains(out, "  - 被折叠内容\n"));
  node(d, root)->collapsed = false;
  node(d, child)->x = 450;
  assert(build_frame(d, f, e));
  assert(f.bounds.x <= 0 && f.bounds.x + f.bounds.w >= 610);
  assert(svg(f, out, e));
  assert(contains(out, "被折叠内容"));
}
int main() {
  structure();
  history_and_copy();
  files();
  capacity_and_layout_failure();
  geometry_and_exports();
  Document wrapped;
  Text error;
  uint64_t id = put(wrapped, "Diagram review");
  node(wrapped, id)->w = 180;
  node(wrapped, id)->h = 80;
  node(wrapped, id)->style.font = 20;
  Frame frame;
  assert(build_frame(wrapped, frame, error));
  assert(frame.labels.size() == 2 &&
         !strcmp(frame.labels[0].text.c_str(), "Diagram") &&
         !strcmp(frame.labels[1].text.c_str(), "review"));
  puts(
      "ReliefDiagram model, layout, history, document and export tests passed");
}
