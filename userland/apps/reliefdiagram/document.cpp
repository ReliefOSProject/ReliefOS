#include "document.h"
namespace rd {
namespace {
uint32_t crc32(const char *p, size_t n) {
  uint32_t c = ~0U;
  for (size_t i = 0; i < n; ++i) {
    c ^= static_cast<unsigned char>(p[i]);
    for (int j = 0; j < 8; ++j)
      c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
  }
  return ~c;
}
struct Writer {
  Bytes b;
  bool ok = true;
  void raw(const char *s, size_t n) {
    if (!ok)
      return;
    size_t at = b.size();
    ok = n <= MaxFileBytes - at && b.resize(at + n);
    if (ok && n)
      memcpy(b.data() + at, s, n);
  }
  void num(uint64_t v, int count) {
    char p[8];
    for (int i = 0; i < count; ++i)
      p[i] = char(v >> (8 * i));
    raw(p, size_t(count));
  }
  void real(double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    num(bits, 8);
  }
  void text(const Text &s) {
    num(s.size(), 4);
    raw(s.c_str(), s.size());
  }
};
struct Reader {
  const char *p;
  size_t size, at = 16;
  bool ok = true;
  uint64_t num(int count) {
    if (!ok || size - at < size_t(count)) {
      ok = false;
      return 0;
    }
    uint64_t n = 0;
    for (int i = 0; i < count; ++i)
      n |= uint64_t(static_cast<unsigned char>(p[at++])) << (8 * i);
    return n;
  }
  double real() {
    uint64_t bits = num(8);
    double v;
    memcpy(&v, &bits, 8);
    return v;
  }
  void text(Text &s, size_t max) {
    size_t n = size_t(num(4));
    if (!ok || n > max || n > size - at || !utf8(p + at, n) ||
        !s.set(p + at, n)) {
      ok = false;
      return;
    }
    at += n;
  }
  bool flag() {
    uint64_t v = num(1);
    if (v > 1)
      ok = false;
    return v != 0;
  }
};
void md_text(Writer &w, const Text &text) {
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text.c_str()[i];
    if (c == '\n' || c == '\t')
      c = ' ';
    if (strchr("\\`*_{}[]()#+-.!|>", c))
      w.raw("\\", 1);
    w.raw(&c, 1);
  }
}
void outline(const Document &d, uint64_t parent, size_t depth, Writer &w) {
  for (size_t i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].parent == parent) {
      for (size_t j = 0; j < depth; ++j)
        w.raw("  ", 2);
      w.raw("- ", 2);
      md_text(w, d.nodes[i].text);
      w.raw("\n", 1);
      if (!d.nodes[i].note.empty()) {
        for (size_t j = 0; j <= depth; ++j)
          w.raw("  ", 2);
        w.raw("> ", 2);
        md_text(w, d.nodes[i].note);
        w.raw("\n", 1);
      }
      outline(d, d.nodes[i].id, depth + 1, w);
    }
}
} // namespace
bool encode(const Document &d, Bytes &out, Text &e) {
  if (!validate(d, e))
    return false;
  Writer w;
  w.raw("RDGM", 4);
  w.num(1, 4);
  w.num(0, 4);
  w.num(0, 4);
  w.text(d.title);
  w.num(d.next_id, 8);
  w.real(d.view.cx);
  w.real(d.view.cy);
  w.real(d.view.zoom);
  w.num(d.view.grid, 1);
  w.num(d.view.snap, 1);
  w.num(d.nodes.size(), 4);
  w.num(d.edges.size(), 4);
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    const Node &n = d.nodes[i];
    w.num(n.id, 8);
    w.num(n.parent, 8);
    w.num(unsigned(n.shape), 1);
    w.real(n.x);
    w.real(n.y);
    w.real(n.w);
    w.real(n.h);
    w.num(n.style.fill, 4);
    w.num(n.style.line, 4);
    w.num(n.style.text, 4);
    w.num(n.style.font, 1);
    w.num(n.style.bold, 1);
    w.num(n.collapsed, 1);
    w.text(n.text);
    w.text(n.note);
  }
  for (size_t i = 0; i < d.edges.size(); ++i) {
    const Edge &a = d.edges[i];
    w.num(a.id, 8);
    w.num(a.from, 8);
    w.num(a.to, 8);
    w.num(unsigned(a.route), 1);
    w.num(a.arrows, 1);
    w.num(a.width, 1);
    w.num(a.color, 4);
    w.text(a.text);
  }
  if (!w.ok) {
    e.set("文档超过 8 MiB 限制或内存不足");
    return false;
  }
  uint32_t header[] = {uint32_t(w.b.size() - 16),
                       crc32(w.b.data() + 16, w.b.size() - 16)};
  for (int n = 0; n < 2; ++n)
    for (int i = 0; i < 4; ++i)
      w.b[8 + n * 4 + i] = char(header[n] >> (8 * i));
  out.swap(w.b);
  e.clear();
  return true;
}
bool decode(const char *p, size_t size, Document &out, Text &e) {
  auto fail = [&]() {
    e.set("无效或损坏的 ReliefDiagram 文档");
    return false;
  };
  if (!p || size < 16 || size > MaxFileBytes || memcmp(p, "RDGM", 4))
    return fail();
  Reader h{p, size, 4};
  uint64_t version = h.num(4), len = h.num(4), crc = h.num(4);
  if (version != 1 || len != size - 16 || crc != crc32(p + 16, size - 16))
    return fail();
  Document d;
  Reader r{p, size};
  r.text(d.title, 512);
  d.next_id = r.num(8);
  d.view.cx = r.real();
  d.view.cy = r.real();
  d.view.zoom = r.real();
  d.view.grid = r.flag();
  d.view.snap = r.flag();
  uint64_t nn = r.num(4), ne = r.num(4);
  if (!r.ok || nn > MaxNodes || ne > MaxEdges || !d.nodes.resize(size_t(nn)) ||
      !d.edges.resize(size_t(ne)))
    return fail();
  for (size_t i = 0; r.ok && i < d.nodes.size(); ++i) {
    Node &n = d.nodes[i];
    n.id = r.num(8);
    n.parent = r.num(8);
    n.shape = Shape(r.num(1));
    n.x = r.real();
    n.y = r.real();
    n.w = r.real();
    n.h = r.real();
    uint64_t fill = r.num(4), line = r.num(4), text = r.num(4);
    n.style.fill = uint32_t(fill);
    n.style.line = uint32_t(line);
    n.style.text = uint32_t(text);
    n.style.font = int(r.num(1));
    n.style.bold = r.flag();
    n.collapsed = r.flag();
    r.text(n.text, 4096);
    r.text(n.note, 16384);
  }
  for (size_t i = 0; r.ok && i < d.edges.size(); ++i) {
    Edge &a = d.edges[i];
    a.id = r.num(8);
    a.from = r.num(8);
    a.to = r.num(8);
    a.route = Route(r.num(1));
    a.arrows = unsigned(r.num(1));
    a.width = unsigned(r.num(1));
    a.color = uint32_t(r.num(4));
    r.text(a.text, 1024);
  }
  if (!r.ok || r.at != size)
    return fail();
  if (!validate(d, e))
    return false;
  out = static_cast<Document &&>(d);
  e.clear();
  return true;
}
bool markdown(const Document &d, Bytes &out, Text &e) {
  if (!validate(d, e))
    return false;
  Writer w;
  w.raw("# ", 2);
  md_text(w, d.title);
  w.raw("\n\n", 2);
  outline(d, 0, 0, w);
  if (!d.edges.empty()) {
    w.raw("\n## 连线\n\n", strlen("\n## 连线\n\n"));
    for (size_t i = 0; i < d.edges.size(); ++i) {
      const Edge &a = d.edges[i];
      w.raw("- ", 2);
      md_text(w, node(d, a.from)->text);
      w.raw(" → ", strlen(" → "));
      md_text(w, node(d, a.to)->text);
      if (!a.text.empty()) {
        w.raw("：", strlen("："));
        md_text(w, a.text);
      }
      w.raw("\n", 1);
    }
  }
  if (!w.ok) {
    e.set("大纲超过限制或内存不足");
    return false;
  }
  out.swap(w.b);
  e.clear();
  return true;
}
} // namespace rd
