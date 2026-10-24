#include "geometry.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace rp;

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }
static bool same_rect(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static float deterministic_measure(void *, const char *bytes, size_t length, const Style &) {
    float width = 0;
    for (size_t at = 0; at < length;) {
        const unsigned char first = static_cast<unsigned char>(bytes[at]);
        size_t count = first < 0x80 ? 1 : ((first & 0xe0) == 0xc0 ? 2 :
            ((first & 0xf0) == 0xe0 ? 3 : 4));
        if (count > length - at) return NAN;
        uint32_t cp = first < 0x80 ? first : first & (count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
        for (size_t i = 1; i < count; ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(bytes[at + i]) & 0x3f);
        if (cp == 0x200d || cp == 0xfe0f || cp == 0x0301 || cp == 0x0308 || cp == 0x1f3fd)
            width += 0;
        else if (cp == '\t') width += 5; // Tabs should arrive as one space.
        else if (cp == ' ') width += 5;
        else if (cp >= 0x2e80 || cp >= 0x1f000) width += 10;
        else width += 5;
        at += count;
    }
    return width;
}

static void test_selection_bounds_and_topmost_hit_testing() {
    Deck deck;
    assert(deck.init());
    Slide &slide = deck.slides[0];
    Object first;
    first.id = 1; first.kind = Kind::Rectangle; first.rect = {10, 20, 30, 40};
    Object second;
    second.id = 2; second.kind = Kind::Ellipse; second.rect = {20, 25, 40, 20};
    assert(slide.objects.push(first) && slide.objects.push(second));
    Array<uint32_t> ids;
    assert(ids.push(1) && ids.push(2) && ids.push(1));
    Bounds bounds = selection_bounds(deck, 0, ids);
    assert(bounds.valid && same_rect(bounds.rect, {10, 20, 50, 40}));
    assert(!selection_bounds(deck, 1, ids).valid);
    assert(hit_test(slide, 25, 30) == 2);
    assert(hit_test(slide, 61, 34) == 0); // Outside ellipse and first rectangle.
    assert(hit_test(slide, NAN, 20) == 0);

    Slide shapes;
    Object ellipse; ellipse.id = 7; ellipse.kind = Kind::Ellipse; ellipse.rect = {0, 0, 100, 60};
    Object line; line.id = 8; line.kind = Kind::Line; line.rect = {10, 80, 100, 20};
    line.style.line_width = 2;
    Object arrow; arrow.id = 9; arrow.kind = Kind::Arrow; arrow.rect = {150, 80, 100, 20};
    assert(shapes.objects.push(ellipse) && shapes.objects.push(line) && shapes.objects.push(arrow));
    assert(hit_test(shapes, 50, 30) == 7);
    assert(hit_test(shapes, 50, 91.5f, 1) == 8);
    assert(hit_test(shapes, 50, 84, 1) == 0);
    assert(hit_test(shapes, 200, 90, 2) == 9);
}

static void test_every_resize_handle_and_page_clamping() {
    const Rect start{100, 100, 80, 40};
    Rect out;
    const float dx[] = {-10, 0, 10, 10, 10, 0, -10, -10};
    const float dy[] = {-5, -5, -5, 0, 5, 5, 5, 0};
    for (int handle = 0; handle < 8; ++handle) {
        assert(resize_rect(start, handle, dx[handle], dy[handle], false, 300, 200, out));
        assert(out.w >= 4 && out.h >= 4);
        assert(out.x >= 0 && out.y >= 0 && out.x + out.w <= 300 && out.y + out.h <= 200);
    }
    assert(resize_rect(start, 0, 1000, 1000, false, 300, 200, out));
    assert(out.w == 4 && out.h == 4);
    assert(resize_rect(start, 4, 1000, 1000, false, 300, 200, out));
    assert(near(out.x + out.w, 300) && near(out.y + out.h, 200));

    assert(resize_rect(start, 3, 20, 0, true, 300, 200, out));
    assert(near(out.w / out.h, 2.0f));
    assert(near(out.y + out.h / 2, 120));
    assert(resize_rect(start, 1, 0, -10, true, 300, 200, out));
    assert(near(out.w / out.h, 2.0f));
    assert(near(out.x + out.w / 2, 140));

    const Rect saved = out;
    assert(!resize_rect(start, 8, 0, 0, false, 300, 200, out));
    assert(same_rect(out, saved));
    assert(!resize_rect({0, 0, NAN, 10}, 0, 0, 0, false, 300, 200, out));
    assert(!resize_rect(start, 0, 0, 0, false, INFINITY, 200, out));
    assert(!resize_rect({290, 180, 20, 20}, 4, 1, 1, false, 300, 200, out));
}

static void test_text_wrap_preserves_ranges_clusters_and_paragraphs() {
    Object object;
    object.kind = Kind::Text;
    object.rect = {0, 0, 46, 200}; // 30 units after horizontal padding.
    assert(object.text.set("hello world", 11));
    Array<TextLine> lines;
    Text error;
    assert(wrap_text(object, deterministic_measure, nullptr, lines, error));
    assert(lines.size() == 2);
    assert(lines[0].start == 0 && lines[0].length == 6 && lines[0].paragraph_start);
    assert(lines[1].start == 6 && lines[1].length == 5 && !lines[1].paragraph_start);
    assert(near(lines[0].width, 30) && near(lines[1].width, 25));

    const char cjk[] = "你好世界";
    object.rect.w = 36; // 20 units: two CJK clusters per line.
    assert(object.text.set(cjk, strlen(cjk)));
    lines.clear();
    assert(wrap_text(object, deterministic_measure, nullptr, lines, error));
    assert(lines.size() == 2 && lines[0].length == 6 && lines[1].start == 6);

    const char clusters[] = "e\xcc\x81👩‍💻x";
    object.rect.w = 36;
    assert(object.text.set(clusters, strlen(clusters)));
    lines.clear();
    assert(wrap_text(object, deterministic_measure, nullptr, lines, error));
    assert(lines.size() >= 2);
    for (size_t i = 0; i < lines.size(); ++i) {
        const size_t end = lines[i].start + lines[i].length;
        assert(lines[i].start != 1 && end != 1);
        assert(lines[i].start != 7 && end != 7);
        assert(lines[i].start != 10 && end != 10); // Never split within the ZWJ cluster.
    }

    const char paragraphs[] = "A\n\nB\n";
    object.rect.w = 80; object.rect.h = 200;
    assert(object.text.set(paragraphs, sizeof(paragraphs) - 1));
    lines.clear();
    assert(wrap_text(object, deterministic_measure, nullptr, lines, error));
    assert(lines.size() == 4);
    assert(lines[0].start == 0 && lines[0].length == 1 && lines[0].paragraph_start);
    assert(lines[1].start == 2 && lines[1].length == 0 && lines[1].paragraph_start);
    assert(lines[2].start == 3 && lines[2].length == 1 && lines[2].paragraph_start);
    assert(lines[3].start == 5 && lines[3].length == 0 && lines[3].paragraph_start);

    object.rect.w = 80;
    assert(object.text.set("a\tb", 3));
    lines.clear();
    assert(wrap_text(object, deterministic_measure, nullptr, lines, error));
    assert(near(lines[0].width, 15)); // Tab normalized to a single measured space.
}

static void test_wrap_rejects_invalid_utf8_measure_and_vertical_overflow_atomically() {
    Object object;
    object.kind = Kind::Text;
    object.style.font_size = 10;
    object.rect = {0, 0, 100, 20};
    assert(object.text.set("one\ntwo", 7));
    Array<TextLine> lines;
    Text error;
    TextLine sentinel; sentinel.start = 91; sentinel.length = 3;
    assert(lines.push(sentinel));
    assert(!wrap_text(object, deterministic_measure, nullptr, lines, error));
    assert(lines.size() == 1 && lines[0].start == 91);
    assert(object.text.set("ok", 2));
    object.rect.w = 10;
    assert(!wrap_text(object, deterministic_measure, nullptr, lines, error));
    const char invalid[] = {char(0xf0), char(0x80), char(0x80), char(0x80)};
    object.rect.w = 100;
    assert(object.text.set(invalid, sizeof(invalid)));
    assert(!wrap_text(object, deterministic_measure, nullptr, lines, error));
    object.text.clear();
    object.rect = {0, 0, 100, 100};
    assert(!wrap_text(object, nullptr, nullptr, lines, error));
}

static void test_wrap_clip_mode_preserves_strict_api_and_source_ranges() {
    Object object;
    object.kind = Kind::Text;
    object.style.font_size = 10;
    object.rect = {0, 0, 20, 4}; // Four content units; the CJK cluster is wider.
    const char narrow[] = "你\nx\n";
    assert(object.text.set(narrow, sizeof(narrow) - 1));
    Array<TextLine> strict_lines;
    Text error;
    TextLine sentinel;
    sentinel.start = 91;
    sentinel.length = 7;
    assert(strict_lines.push(sentinel));
    assert(!wrap_text(object, deterministic_measure, nullptr, strict_lines, error));
    assert(strict_lines.size() == 1 && strict_lines[0].start == 91);
    assert(!wrap_text(object, deterministic_measure, nullptr, strict_lines, error, false));
    assert(strict_lines.size() == 1 && strict_lines[0].start == 91);

    Array<TextLine> clipped;
    assert(wrap_text(object, deterministic_measure, nullptr, clipped, error, true));
    assert(clipped.size() == 3);
    assert(clipped[0].start == 0 && clipped[0].length == 3 && near(clipped[0].width, 10));
    assert(clipped[1].start == 4 && clipped[1].length == 1);
    assert(clipped[2].start == 6 && clipped[2].length == 0);
    assert(clipped[0].paragraph_start && clipped[1].paragraph_start && clipped[2].paragraph_start);

    object.rect = {0, 0, 100, 20};
    assert(object.text.set("one\ntwo", 7));
    strict_lines.clear();
    assert(!wrap_text(object, deterministic_measure, nullptr, strict_lines, error));
    assert(strict_lines.empty());
    assert(wrap_text(object, deterministic_measure, nullptr, strict_lines, error, true));
    assert(strict_lines.size() == 2);
    assert(strict_lines[0].start == 0 && strict_lines[0].length == 3);
    assert(strict_lines[1].start == 4 && strict_lines[1].length == 3);

    object.rect.w = 16; // No content remains after the 8-unit side paddings.
    assert(object.text.set("x", 1));
    TextLine old_line;
    old_line.start = 7;
    assert(strict_lines.push(old_line));
    assert(!wrap_text(object, deterministic_measure, nullptr, strict_lines, error));
    assert(wrap_text(object, deterministic_measure, nullptr, strict_lines, error, true));
    assert(strict_lines.empty());
}

int main() {
    test_selection_bounds_and_topmost_hit_testing();
    test_every_resize_handle_and_page_clamping();
    test_text_wrap_preserves_ranges_clusters_and_paragraphs();
    test_wrap_rejects_invalid_utf8_measure_and_vertical_overflow_atomically();
    test_wrap_clip_mode_preserves_strict_api_and_source_ranges();
    puts("ReliefPPT geometry tests passed");
    return 0;
}
