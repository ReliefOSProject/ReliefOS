// Independent host process: fixtures and exact ReliefPPT guest readback.
#include "document.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
using namespace rp;
namespace {
int failures;
void check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool add_text(Editor &e, const char *text, Rect r, float size, bool bold = false, unsigned family = 0) {
    Object o;
    o.kind = Kind::Text;
    o.rect = r;
    o.style.font_size = size;
    o.style.bold = bold;
    o.style.family = uint8_t(family);
    o.style.text = 0x233c59;
    return o.text.set(text) && e.insert_object(o);
}
bool prepare(const char *dir) {
    Editor e;
    if (!add_text(e, "ReliefPPT 演示文稿", {72, 52, 816, 112}, 42, true))
        return false;
    if (!add_text(e, "在 ReliefOS 中展示你的想法\n文字、图形和图片自由排版\n原生保存，随时继续编辑",
                  {72, 250, 610, 150}, 28))
        return false;
    Object shape;
    shape.kind = Kind::Rectangle;
    shape.rect = {72, 432, 816, 18};
    shape.style.filled = true;
    shape.style.fill = 0x3478b8;
    if (!e.insert_object(shape))
        return false;
    unsigned char rgba[32 * 32 * 4];
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x) {
            size_t n = (y * 32 + x) * 4;
            rgba[n] = 40 + x * 5;
            rgba[n + 1] = 120 + y * 3;
            rgba[n + 2] = 190;
            rgba[n + 3] = (x < 3 || y < 3) ? 0 : 255;
        }
    if (!e.insert_image(32, 32, reinterpret_cast<const char *>(rgba), sizeof(rgba), {728, 230, 136, 136}))
        return false;
    if (!e.add_slide(Layout::Blank) || !add_text(e, "清晰地讲述你的计划", {64, 45, 832, 90}, 38, true) ||
        !add_text(e, "第一步：写下核心观点\n第二步：加入图片与图形\n第三步：按 F5 开始放映",
                  {80, 175, 800, 220}, 32, false, 1))
        return false;
    if (!e.set_notes("这段备注只在编辑器显示。", strlen("这段备注只在编辑器显示。")))
        return false;
    if (!e.add_slide(Layout::Blank) || !add_text(e, "谢谢观看", {150, 180, 660, 130}, 64, true, 2))
        return false;
    e.current = 0;
    Text error;
    char path[4096];
    snprintf(path, sizeof(path), "%s/ppt-fixture.rppt", dir);
    if (!save_deck(path, e.deck, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    Editor overflow;
    if (!add_text(overflow, "第一行保持可见\n第二行超出高度\n第三行在框内裁剪", {72, 52, 400, 70}, 32) ||
        !add_text(overflow, "中文窄框", {72, 300, 22, 100}, 32) ||
        !add_text(overflow, "极小框", {500, 300, 8, 8}, 32))
        return false;
    Object later;
    later.kind = Kind::Rectangle;
    later.rect = {600, 120, 140, 110};
    later.style.filled = true;
    later.style.fill = 0xe05a40;
    if (!overflow.insert_object(later))
        return false;
    snprintf(path, sizeof(path), "%s/overflow-fixture.rppt", dir);
    if (!save_deck(path, overflow.deck, error))
        return false;
    return true;
}
void verify(const char *path, const char *mode) {
    Deck d;
    Text error;
    if (!load_deck(path, d, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        ++failures;
        return;
    }
    check(d.validate(error), "native deck validates");
    if (!strcmp(mode, "branch") || !strcmp(mode, "branch-a")) {
        check(d.slides.size() == 1 && d.slides[0].objects.size() == 1 &&
                  !strcmp(d.slides[0].objects[0].text.c_str(), !strcmp(mode, "branch-a") ? "A" : "B"),
              "saved undo branch is the exact new edit");
        return;
    }
    if (!strcmp(mode, "images")) {
        check(d.slides.size() == 1 && d.slides[0].objects.size() == 2 && d.assets.size() == 2,
              "both PNG and BMP imports saved");
        if (d.assets.size() == 2) {
            const Asset &png = d.assets[0], &bmp = d.assets[1];
            check(png.width == 32 && png.height == 24 && png.rgba.size() == 32 * 24 * 4,
                  "PNG import dimensions");
            check(png.rgba.size() >= 20 && !png.rgba.c_str()[3] &&
                      static_cast<unsigned char>(png.rgba.c_str()[19]) == 255,
                  "PNG alpha preserved");
            check(bmp.width == 16 && bmp.height == 12 && bmp.rgba.size() >= 4 &&
                      static_cast<unsigned char>(bmp.rgba.c_str()[0]) == 220 &&
                      static_cast<unsigned char>(bmp.rgba.c_str()[1]) == 100 &&
                      static_cast<unsigned char>(bmp.rgba.c_str()[2]) == 20 &&
                      static_cast<unsigned char>(bmp.rgba.c_str()[3]) == 255,
                  "BMP channels and opaque alpha preserved");
        }
        return;
    }
    check(d.slides.size() >= 3, "original slides preserved");
    check(d.assets.size() > 0, "embedded image retained");
    bool chinese = false, digits = false, recovery_first = false, recovery_second = false;
    unsigned images = 0, shapes = 0;
    for (size_t s = 0; s < d.slides.size(); ++s)
        for (size_t i = 0; i < d.slides[s].objects.size(); ++i) {
            const Object &o = d.slides[s].objects[i];
            if (o.kind == Kind::Image)
                ++images;
            if (o.kind == Kind::Rectangle || o.kind == Kind::Ellipse)
                ++shapes;
            if (strstr(o.text.c_str(), "中文输入框粘贴测试"))
                chinese = true;
            if (strstr(o.text.c_str(), "PptCheck: 123456789"))
                digits = true;
            if (strstr(o.text.c_str(), "Recovery marker 2468"))
                recovery_first = true;
            if (strstr(o.text.c_str(), "Recovery marker 8642"))
                recovery_second = true;
        }
    check(images > 0 && shapes > 0, "embedded graphic and shape preserved");
    if (strcmp(mode, "fixture")) {
        check(chinese, "actual UTF8 paste saved");
        check(digits, "fast input saved exactly");
    }
    if (!strcmp(mode, "recovery")) {
        check(recovery_first, "first crashed unsaved edit survived recovery");
        check(recovery_second, "second-generation autosave survived another crash and recovery");
    }
    if (!strcmp(mode, "edited")) {
        check(d.slides.size() == 4, "duplicated slide retained");
        check(d.slides[0].objects.size() == 4, "clipboard undo removed the temporary duplicate");
        const Object *title = d.object(0, 1);
        check(title && title->style.bold && fabs(title->style.font_size - 36) < .01,
              "title font control persisted");
        check(title &&
                  !strcmp(title->text.c_str(), "ReliefPPT 演示文稿\n中文输入框粘贴测试\nPptCheck: 123456789"),
              "edited title text matches exactly");
        check(title && fabs(title->rect.h - 180) < .01, "object properties persisted");
        const Object *image = d.object(0, 4);
        check(image && fabs(image->rect.x - 768) < 3 && fabs(image->rect.y - 250) < 3 &&
                  fabs(image->rect.w - 166) < 3 && fabs(image->rect.h - 156) < 3,
              "actual image drag and handle resize persisted");
        check(d.slides.size() == 4 && strstr(d.slides[1].objects[0].text.c_str(), "清晰地") &&
                  strstr(d.slides[2].objects[0].text.c_str(), "ReliefPPT"),
              "duplicate moved after the second slide");
        check(d.slides.size() > 1 && !strcmp(d.slides[1].notes.c_str(), "这段备注只在编辑器显示。"),
              "speaker notes preserved by sorting and saving");
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    if (!strcmp(argv[1], "prepare"))
        return prepare(argv[2]) ? 0 : 1;
    verify(argv[1], argv[2]);
    if (!failures)
        puts("ReliefPPT guest readback passed");
    return failures ? 1 : 0;
}
