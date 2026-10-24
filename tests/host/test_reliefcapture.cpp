#include "../../userland/apps/reliefcapture/model.h"
#include <assert.h>
#include <stdio.h>
using namespace rc;
int main() {
  Rect r = selection(60, 80, -20, -5, 100, 100);
  assert(r.x == 0 && r.y == 0 && r.w == 60 && r.h == 80);
  assert(r.contains(0, 0) && !r.contains(60, 79));
  assert(!selection(3, 4, 3, 8, 100, 100).valid());
  assert(!selection(1, 2, 3, 4, -1, 5).valid());
  r = fit_rect(400, 200, {10, 20, 100, 100});
  assert(r.x == 10 && r.y == 45 && r.w == 100 && r.h == 50);
  Point p;
  assert(image_point(r, 400, 200, 109, 94, p) && p.x == 396 && p.y == 196);
  assert(!image_point(r, 400, 200, 110, 94, p));
  Image image, crop;
  assert(!image.create(0, 10) && !image.create(8193, 1));
  assert(!image.create(8192, 8192));
  assert(image.create(10, 8));
  for (size_t n = 0; n < image.pixels.size(); ++n) image.pixels[n] = unsigned(n);
  assert(image.crop({2, 3, 4, 2}, crop));
  assert(crop.width == 4 && crop.height == 2 && crop.pixels[0] == 32 && crop.pixels[7] == 45);
  assert(!image.crop({9, 0, 3, 1}, crop) && crop.pixels[0] == 32);
  assert(image.crop({0, 0, 2, 2}, image) && image.pixels[3] == 11);
  assert(valid_utf8("截图", 6));
  assert(!valid_utf8("\xc0\x80", 2) && !valid_utf8("\xed\xa0\x80", 3));
  assert(!valid_utf8("\xe4\xb8", 2) && !valid_utf8("a\0b", 3));
  Document doc;
  assert(image.create(100, 100)); doc.replace(static_cast<Image &&>(image));
  Mark mark; mark.tool = Tool::Arrow; mark.a = {5, 7}; mark.b = {50, 60};
  assert(doc.add(static_cast<Mark &&>(mark)));
  assert(doc.visible() == 1 && doc.undo() && doc.can_redo() && doc.redo());
  assert(doc.undo());
  Mark replacement; replacement.tool = Tool::Rectangle; replacement.b = {10, 15};
  assert(doc.add(static_cast<Mark &&>(replacement)) && !doc.can_redo());
  Mark outside; outside.a = {-1, 0}; outside.points.push({1, 1});
  assert(!doc.add(static_cast<Mark &&>(outside)) && doc.visible() == 1);
  Mark long_pen; assert(long_pen.points.resize(MaxPoints + 1));
  assert(!doc.add(static_cast<Mark &&>(long_pen)));
  Mark text; text.tool = Tool::Label; text.text.set("文字");
  assert(doc.add(static_cast<Mark &&>(text)));
  for (size_t n = doc.visible(); n < MaxMarks; ++n) {
    Mark next; next.tool = Tool::Arrow; next.b = {2, 3};
    assert(doc.add(static_cast<Mark &&>(next)));
  }
  Mark excess; excess.tool = Tool::Arrow; excess.b = {2, 3};
  assert(!doc.add(static_cast<Mark &&>(excess)) && doc.visible() == MaxMarks);
  assert(doc.undo());
  Mark after_undo; after_undo.tool = Tool::Arrow; after_undo.b = {3, 4};
  assert(doc.add(static_cast<Mark &&>(after_undo)) && !doc.can_redo());
  puts("ReliefCapture: selection, pixel mapping, crop bounds, UTF-8 and annotation history passed");
}
