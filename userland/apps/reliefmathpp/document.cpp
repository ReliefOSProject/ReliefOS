#include "document.h"
namespace rmp {
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
bool utf8(const char *s, size_t n) {
  for (size_t i = 0; i < n;) {
    unsigned c = static_cast<unsigned char>(s[i++]);
    if (!c)
      return false;
    if (c < 128)
      continue;
    unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xf0 && c <= 0xf4 ? 3
                                              : 99;
    unsigned value = 0;
    if (count == 99 || count > n - i)
      return false;
    value = c & ((1U << (6 - count)) - 1);
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
struct Writer {
  Bytes b;
  bool raw(const char *s, size_t n) {
    size_t pos = b.size();
    if (n > MaxFileBytes - pos || !b.resize(pos + n))
      return false;
    if (n)
      memcpy(b.data() + pos, s, n);
    return true;
  }
  bool num(uint64_t x, int n) {
    char p[8];
    for (int i = 0; i < n; ++i)
      p[i] = char(x >> (8 * i));
    return raw(p, size_t(n));
  }
  bool real(double d) {
    uint64_t x;
    memcpy(&x, &d, 8);
    return num(x, 8);
  }
  bool text(const Text &s) {
    return utf8(s.c_str(), s.size()) && num(s.size(), 4) &&
           raw(s.c_str(), s.size());
  }
};
struct Reader {
  const char *p;
  size_t size, at = 16;
  bool num(uint64_t &x, int n) {
    if (size - at < size_t(n))
      return false;
    x = 0;
    for (int i = 0; i < n; ++i)
      x |= uint64_t(static_cast<unsigned char>(p[at++])) << (8 * i);
    return true;
  }
  bool real(double &d) {
    uint64_t x;
    if (!num(x, 8))
      return false;
    memcpy(&d, &x, 8);
    return true;
  }
  bool text(Text &s, size_t limit) {
    uint64_t n;
    if (!num(n, 4) || n > limit || n > size - at || !utf8(p + at, size_t(n)) ||
        !s.set(p + at, size_t(n)))
      return false;
    at += size_t(n);
    return true;
  }
};
} // namespace
bool encode(const Document &d, Bytes &out, Text &err) {
  if (!validate(d, err))
    return false;
  Writer w;
  bool ok = w.raw("RMPP", 4) && w.num(1, 4) && w.num(0, 4) && w.num(0, 4) &&
            w.num(d.next_id, 8) && w.num(d.objects.size(), 4);
  for (int n = 0; n < 10; ++n)
    ok = ok && w.real(d.vars.get(n));
  const View &v = d.view;
  double values[] = {v.cx,     v.cy, v.span, v.yaw, v.pitch,
                     v.scale3, v.tx, v.ty,   v.tz};
  for (double x : values)
    ok = ok && w.real(x);
  ok = ok && w.num((v.grid ? 1 : 0) | (v.axes ? 2 : 0) | (v.wire ? 4 : 0), 1) &&
       w.num(v.tab, 1);
  for (size_t n = 0; ok && n < d.objects.size(); ++n) {
    const Object &o = d.objects[n];
    ok = w.num(o.id, 8) && w.num(unsigned(o.kind), 1) &&
         w.num(o.visible ? 1 : 0, 1) && w.num(o.color, 1) &&
         w.num(o.resolution, 1) && w.text(o.name);
    for (const Text &s : o.formula)
      ok = ok && w.text(s);
    for (double x : o.p)
      ok = ok && w.real(x);
    for (double x : o.range)
      ok = ok && w.real(x);
  }
  if (!ok) {
    err.set("文档超出限制、文本无效或内存不足");
    return false;
  }
  uint32_t size = uint32_t(w.b.size()),
           crc = crc32(w.b.data() + 16, w.b.size() - 16);
  for (int n = 0; n < 4; ++n) {
    w.b[8 + n] = char(size >> (8 * n));
    w.b[12 + n] = char(crc >> (8 * n));
  }
  out.swap(w.b);
  err.clear();
  return true;
}
bool decode(const char *p, size_t size, Document &out, Text &err) {
  if (!p || size < 28 || size > MaxFileBytes || memcmp(p, "RMPP", 4)) {
    err.set("不是 ReliefMath++ 文档或文件已截断");
    return false;
  }
  Reader r{p, size, 4};
  uint64_t version = 0, length = 0, crc = 0, count = 0, flags = 0, tab = 0;
  Document d;
  bool ok = r.num(version, 4) && version == 1 && r.num(length, 4) &&
            length == size && r.num(crc, 4) &&
            crc == crc32(p + 16, size - 16) && r.num(d.next_id, 8) &&
            r.num(count, 4) && count <= MaxObjects;
  double vars[10]{};
  for (double &x : vars)
    ok = ok && r.real(x);
  d.vars = {vars[0], vars[1], vars[2], vars[3], vars[4],
            vars[5], vars[6], vars[7], vars[8], vars[9]};
  View &v = d.view;
  double *values[] = {&v.cx,     &v.cy, &v.span, &v.yaw, &v.pitch,
                      &v.scale3, &v.tx, &v.ty,   &v.tz};
  for (double *x : values)
    ok = ok && r.real(*x);
  ok = ok && r.num(flags, 1) && flags <= 7 && r.num(tab, 1) && tab <= 1;
  v.grid = flags & 1;
  v.axes = flags & 2;
  v.wire = flags & 4;
  v.tab = int(tab);
  for (size_t n = 0; ok && n < count; ++n) {
    Object o;
    uint64_t k, visible, color, res;
    ok = r.num(o.id, 8) && r.num(k, 1) && k < unsigned(Kind::Count) &&
         r.num(visible, 1) && visible <= 1 && r.num(color, 1) &&
         r.num(res, 1) && r.text(o.name, 128);
    if (!ok)
      break;
    o.kind = Kind(k);
    o.visible = visible;
    o.color = int(color);
    o.resolution = int(res);
    for (Text &s : o.formula)
      ok = ok && r.text(s, 1024);
    for (double &x : o.p)
      ok = ok && r.real(x);
    for (double &x : o.range)
      ok = ok && r.real(x);
    ok = ok && d.objects.push(o);
  }
  if (!ok || r.at != size) {
    err.set("文档校验失败、版本不支持或内容损坏");
    return false;
  }
  if (!validate(d, err))
    return false;
  out = static_cast<Document &&>(d);
  err.clear();
  return true;
}
} // namespace rmp
