#include "../../userland/apps/reliefword/layout.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace rw;

static Metrics deterministic_measure(void *, uint32_t codepoint,
                                     const TextStyle &style) {
    Metrics result;
    const float size = static_cast<float>(style.size);
    result.advance = ((codepoint >= 0x2e80 && codepoint <= 0x9fff) ||
                      (codepoint >= 0xff00 && codepoint <= 0xffef) ||
                      (codepoint >= 0x1f000 && codepoint <= 0x1faff))
                         ? size : size / 2.0f;
    if (codepoint == '\t') result.advance = 12.0f;
    if (codepoint == 0x0301 || codepoint == 0x200d || codepoint == 0xfe0f ||
        codepoint == 0x2060)
        result.advance = 0;
    result.ascent = size * 0.8f;
    result.descent = size * 0.2f;
    return result;
}

static void start_document(Document &doc, const PageSettings &page = PageSettings{}) {
    TextStyle style;
    style.size = 10;
    assert(doc.styles.push(style));
    Paragraph paragraph;
    paragraph.start = 0;
    assert(doc.paragraphs.push(paragraph));
    doc.page = page;
}

static void put_glyph(Document &doc, uint32_t codepoint, uint16_t style = 0) {
    Glyph glyph;
    glyph.codepoint = codepoint;
    glyph.style = style;
    assert(doc.chars.push(glyph));
}

static void put_paragraph(Document &doc, size_t start, const ParagraphStyle &style) {
    Paragraph paragraph;
    paragraph.start = start;
    paragraph.style = style;
    assert(doc.paragraphs.push(paragraph));
}

static PageSettings small_page(uint16_t width = 100, uint16_t height = 100) {
    PageSettings page;
    page.width = width;
    page.height = height;
    page.left = page.right = 10;
    page.top = page.bottom = 10;
    return page;
}

static bool close_to(float actual, float expected) {
    return fabsf(actual - expected) < 0.001f;
}

static bool is_model_cluster_boundary(const Document &doc, size_t offset) {
    size_t boundary = 0;
    while (boundary < offset) {
        const size_t next = next_cluster(doc, boundary);
        if (next <= boundary) return false;
        boundary = next;
    }
    return boundary == offset;
}

static void test_builds_empty_document_with_page_relative_caret() {
    Document doc;
    start_document(doc, small_page());
    Layout layout;
    assert(layout.build(doc, deterministic_measure));
    assert(layout.pages == 1);
    assert(layout.lines.size() == 1);
    assert(layout.boxes.size() == 1);
    assert(layout.lines[0].start == 0 && layout.lines[0].end == 0);
    assert(layout.lines[0].page == 0);
    assert(layout.lines[0].x == 10.0f);
    assert(layout.lines[0].y == 10.0f);
    assert(layout.lines[0].height == 12.0f);
    assert(layout.lines[0].baseline == 18.0f);
    assert(layout.boxes[0].line == 0);
    assert(layout.boxes[0].x == 10.0f);
    assert(layout.line_for(0) == 0);
    assert(layout.caret_x(0, 0) == 10.0f);
    assert(layout.hit_test(0, 10.0f, 10.0f) == 0);
}

static void test_places_glyphs_and_nearest_carets_on_one_line() {
    Document doc;
    start_document(doc, small_page());
    put_glyph(doc, 'A');
    put_glyph(doc, 'B');
    Layout layout;
    assert(layout.build(doc, deterministic_measure));
    assert(layout.lines.size() == 1);
    assert(layout.lines[0].start == 0 && layout.lines[0].end == 2);
    assert(layout.lines[0].width == 10.0f);
    assert(layout.boxes.size() == 3);
    assert(layout.boxes[0].x == 10.0f && layout.boxes[0].line == 0);
    assert(layout.boxes[1].x == 15.0f && layout.boxes[1].line == 0);
    assert(layout.boxes[2].x == 20.0f && layout.boxes[2].line == 0);
    assert(layout.line_for(2) == 0);
    assert(layout.caret_x(1, 0) == 15.0f);
    assert(layout.hit_test(0, 18.0f, 12.0f) == 2);
}

static void test_wraps_latin_words_at_spaces_and_long_words_at_clusters() {
    Document doc;
    start_document(doc, small_page(50));
    const uint32_t words[] = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    for (uint32_t cp : words) put_glyph(doc, cp);
    Layout layout;
    assert(layout.build(doc, deterministic_measure));
    assert(layout.lines.size() == 2);
    assert(layout.lines[0].start == 0 && layout.lines[0].end == 6);
    assert(layout.lines[1].start == 6 && layout.lines[1].end == 11);
    assert(layout.boxes[6].line == 1);
    assert(layout.line_for(6) == 1);
    assert(layout.line_for(6, true) == 0);
    assert(close_to(layout.caret_x(6, 0), layout.lines[0].x + layout.lines[0].width));
    assert(layout.hit_test(0, layout.lines[1].x + 24.0f, layout.lines[1].y + 3.0f) == 11);

    Document long_word;
    start_document(long_word, small_page(30));
    const uint32_t letters[] = {'a', 'b', 'c', 'd', 'e'};
    for (uint32_t cp : letters) put_glyph(long_word, cp);
    Layout fallback;
    assert(fallback.build(long_word, deterministic_measure));
    assert(fallback.lines.size() == 3);
    assert(fallback.lines[0].start == 0 && fallback.lines[0].end == 2);
    assert(fallback.lines[1].start == 2 && fallback.lines[1].end == 4);
    assert(fallback.lines[2].start == 4 && fallback.lines[2].end == 5);
}

static void test_preserves_chinese_punctuation_combining_and_emoji_clusters() {
    PageSettings punctuation_page = small_page(35);
    punctuation_page.left = punctuation_page.right = 5;
    Document closing;
    start_document(closing, punctuation_page);
    put_glyph(closing, 0x7532); // 甲
    put_glyph(closing, 0x4e59); // 乙
    put_glyph(closing, 0xff0c); // ，
    put_glyph(closing, 0x4e19); // 丙
    Layout closing_layout;
    assert(closing_layout.build(closing, deterministic_measure));
    assert(closing_layout.lines.size() == 3);
    for (size_t i = 0; i < closing_layout.lines.size(); ++i)
        assert(closing.chars[closing_layout.lines[i].start].codepoint != 0xff0c);

    Document opening;
    start_document(opening, punctuation_page);
    put_glyph(opening, 0x7532); // 甲
    put_glyph(opening, 0xff08); // （
    put_glyph(opening, 0x4e59); // 乙
    Layout opening_layout;
    assert(opening_layout.build(opening, deterministic_measure));
    assert(opening_layout.lines.size() == 2);
    assert(opening.chars[opening_layout.lines[0].end - 1].codepoint != 0xff08);

    Document combining;
    start_document(combining, small_page(30));
    put_glyph(combining, 'e');
    put_glyph(combining, 0x0301);
    put_glyph(combining, 'x');
    put_glyph(combining, 'y');
    Layout combining_layout;
    assert(combining_layout.build(combining, deterministic_measure));
    for (size_t i = 0; i < combining_layout.lines.size(); ++i) {
        const Line &line = combining_layout.lines[i];
        assert(line.start != 1);
        assert(line.end != 1);
    }

    Document emoji;
    start_document(emoji, small_page(30));
    put_glyph(emoji, 0x1f469);
    put_glyph(emoji, 0x200d);
    put_glyph(emoji, 0x1f469);
    put_glyph(emoji, 0x200d);
    put_glyph(emoji, 0x1f469);
    put_glyph(emoji, 0x200d);
    put_glyph(emoji, 0x1f469);
    put_glyph(emoji, 'x');
    Layout emoji_layout;
    assert(emoji_layout.build(emoji, deterministic_measure));
    assert(emoji_layout.lines.size() == 2);
    assert(emoji_layout.lines[0].start == 0 && emoji_layout.lines[0].end == 7);
    assert(emoji_layout.lines[1].start == 7 && emoji_layout.lines[1].end == 8);
}

static void test_hit_test_never_places_caret_inside_grapheme_clusters() {
    const uint32_t cases[][3] = {
        {'e', 0x0301, 0},       // combining mark
        {0x1f469, 0xfe0f, 0},   // variation selector
        {0x1f469, 0x1f3fd, 0},  // emoji modifier
        {0x1f469, 0x200d, 0x1f4bb}, // ZWJ sequence
        {0x1f1fa, 0x1f1f8, 0}, // regional-indicator pair
    };
    const size_t case_lengths[] = {2, 2, 2, 3, 2};
    for (size_t c = 0; c < sizeof(case_lengths) / sizeof(case_lengths[0]); ++c) {
        Document doc;
        start_document(doc, small_page());
        for (size_t i = 0; i < case_lengths[c]; ++i) put_glyph(doc, cases[c][i]);
        Layout layout;
        assert(layout.build(doc, deterministic_measure));
        assert(layout.lines.size() == 1);
        assert(layout.grapheme_boundaries.size() == doc.chars.size() + 1);
        assert(layout.grapheme_boundaries[0] == 1);
        assert(layout.grapheme_boundaries[case_lengths[c]] == 1);

        // Every constituent codepoint after the first has an internal box, but
        // clicking there must resolve to one of the model's grapheme edges.
        for (size_t internal = 1; internal < case_lengths[c]; ++internal) {
            assert(layout.grapheme_boundaries[internal] == 0);
            const float x = layout.boxes[internal].x;
            const size_t hit = layout.hit_test(0, x, layout.lines[0].y + 1.0f);
            assert(is_model_cluster_boundary(doc, hit));
            assert(hit == 0 || hit == case_lengths[c]);
            const float caret = layout.caret_x(hit, layout.line_for(hit));
            const float box_caret = hit == layout.lines[0].end
                                        ? layout.lines[0].x + layout.lines[0].width
                                        : layout.boxes[hit].x;
            assert(close_to(caret, box_caret));
        }
    }
}

static void test_nonbreaking_glue_and_tabs_keep_measured_positions() {
    Document no_break_space;
    start_document(no_break_space, small_page(35));
    const uint32_t nbsp_text[] = {'x', ' ', 'A', 0x00a0, 'B'};
    for (uint32_t cp : nbsp_text) put_glyph(no_break_space, cp);
    Layout nbsp_layout;
    assert(nbsp_layout.build(no_break_space, deterministic_measure));
    assert(nbsp_layout.lines.size() == 2);
    assert(nbsp_layout.lines[0].end == 2);
    assert(nbsp_layout.lines[1].start == 2);

    Document word_joiner;
    start_document(word_joiner, small_page(35));
    const uint32_t wj_text[] = {'x', ' ', 'A', 0x2060, 'B'};
    for (uint32_t cp : wj_text) put_glyph(word_joiner, cp);
    Layout wj_layout;
    assert(wj_layout.build(word_joiner, deterministic_measure));
    assert(wj_layout.lines.size() == 2);
    assert(wj_layout.lines[0].end == 2 && wj_layout.lines[1].start == 2);

    Document tabs;
    start_document(tabs, small_page(80));
    put_glyph(tabs, 'A');
    put_glyph(tabs, '\t');
    put_glyph(tabs, 'B');
    Layout tab_layout;
    assert(tab_layout.build(tabs, deterministic_measure));
    assert(tab_layout.lines.size() == 1);
    assert(tab_layout.boxes[0].x == 10.0f);
    assert(tab_layout.boxes[1].x == 15.0f);
    assert(tab_layout.boxes[2].x == 27.0f);
    assert(tab_layout.boxes[3].x == 32.0f);
}

static void test_mixed_font_metrics_alignment_and_justification() {
    Document centered;
    start_document(centered, small_page());
    TextStyle large;
    large.size = 20;
    assert(centered.styles.push(large));
    put_glyph(centered, 'A');
    put_glyph(centered, 0x4e2d, 1);
    centered.paragraphs[0].style.alignment = Alignment::Center;
    Layout center_layout;
    assert(center_layout.build(centered, deterministic_measure));
    assert(center_layout.lines.size() == 1);
    assert(close_to(center_layout.lines[0].width, 25.0f));
    assert(close_to(center_layout.lines[0].x, 37.5f));
    assert(close_to(center_layout.lines[0].height, 24.0f));
    assert(close_to(center_layout.lines[0].baseline, 26.0f));

    centered.paragraphs[0].style.alignment = Alignment::Right;
    Layout right_layout;
    assert(right_layout.build(centered, deterministic_measure));
    assert(close_to(right_layout.lines[0].x, 65.0f));
    centered.paragraphs[0].style.line_spacing = 150;
    Layout spaced_layout;
    assert(spaced_layout.build(centered, deterministic_measure));
    assert(close_to(spaced_layout.lines[0].height, 30.0f));

    Document justified;
    start_document(justified, small_page(45));
    justified.paragraphs[0].style.alignment = Alignment::Justify;
    put_glyph(justified, 0x7532);
    put_glyph(justified, 0x4e59);
    put_glyph(justified, 0x4e19);
    put_glyph(justified, 0x4e01);
    Layout justify_layout;
    assert(justify_layout.build(justified, deterministic_measure));
    assert(justify_layout.lines.size() == 2);
    assert(close_to(justify_layout.lines[0].width, 25.0f));
    assert(close_to(justify_layout.boxes[1].x, 25.0f));
    assert(close_to(justify_layout.lines[1].width, 20.0f));

    Document indented;
    start_document(indented, small_page(60));
    indented.paragraphs[0].style.left_indent = 5;
    indented.paragraphs[0].style.right_indent = 5;
    indented.paragraphs[0].style.first_indent = 10;
    const uint32_t word[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    for (uint32_t cp : word) put_glyph(indented, cp);
    Layout indent_layout;
    assert(indent_layout.build(indented, deterministic_measure));
    assert(indent_layout.lines.size() == 2);
    assert(indent_layout.lines[0].start == 0 && indent_layout.lines[0].end == 4);
    assert(indent_layout.lines[0].x == 25.0f);
    assert(indent_layout.lines[1].start == 4 && indent_layout.lines[1].x == 15.0f);
    assert(indent_layout.lines[1].x + indent_layout.lines[1].width <= 45.0f);
}

static void test_paragraph_spacing_lists_and_trailing_empty_paragraph() {
    Document doc;
    start_document(doc, small_page());
    ParagraphStyle first = doc.paragraphs[0].style;
    first.list = ListKind::Number;
    first.after = 6;
    doc.paragraphs[0].style = first;
    put_glyph(doc, 'A');
    put_glyph(doc, '\n');

    ParagraphStyle second;
    second.before = 4;
    second.list = ListKind::Number;
    put_paragraph(doc, 2, second);
    put_glyph(doc, 'B');
    put_glyph(doc, '\n');

    ParagraphStyle third;
    third.list = ListKind::Bullet;
    put_paragraph(doc, 4, third);
    put_glyph(doc, 'C');
    put_glyph(doc, '\n');

    ParagraphStyle fourth;
    fourth.list = ListKind::Number;
    put_paragraph(doc, 6, fourth);
    put_glyph(doc, 'D');

    Layout layout;
    assert(layout.build(doc, deterministic_measure));
    assert(layout.lines.size() == 4);
    assert(layout.lines[0].list_number == 1);
    assert(layout.lines[1].list_number == 2);
    assert(layout.lines[2].list_number == 0);
    assert(layout.lines[3].list_number == 1);
    assert(layout.lines[0].x == 25.0f);
    assert(layout.lines[2].x == 20.0f);
    assert(close_to(layout.lines[1].y,
                    layout.lines[0].y + layout.lines[0].height + 6.0f + 4.0f));
    assert(layout.boxes.size() == 8);
    assert(layout.boxes[1].line == 0); // caret before LF remains on the prior line
    assert(layout.boxes[2].line == 1);
    assert(layout.line_for(1) == 0);
    assert(layout.line_for(2) == 1);
    assert(layout.boxes[7].line == 3);

    Document trailing;
    start_document(trailing, small_page());
    put_glyph(trailing, 'A');
    put_glyph(trailing, '\n');
    put_paragraph(trailing, 2, ParagraphStyle{});
    Layout trailing_layout;
    assert(trailing_layout.build(trailing, deterministic_measure));
    assert(trailing_layout.lines.size() == 2);
    assert(trailing_layout.lines[1].start == 2 && trailing_layout.lines[1].end == 2);
    assert(trailing_layout.boxes.size() == 3);
    assert(trailing_layout.boxes[2].line == 1);
}

static void test_page_overflow_manual_breaks_and_atomic_rejection() {
    PageSettings short_page = small_page(100, 40);
    short_page.top = short_page.bottom = 5;
    Document paragraphs;
    start_document(paragraphs, short_page);
    ParagraphStyle spaced;
    spaced.after = 6;
    paragraphs.paragraphs[0].style = spaced;
    put_glyph(paragraphs, 'A');
    put_glyph(paragraphs, '\n');
    put_paragraph(paragraphs, 2, spaced);
    put_glyph(paragraphs, 'B');
    put_glyph(paragraphs, '\n');
    put_paragraph(paragraphs, 4, spaced);
    put_glyph(paragraphs, 'C');
    put_glyph(paragraphs, '\n');
    put_paragraph(paragraphs, 6, spaced);
    put_glyph(paragraphs, 'D');
    Layout automatic;
    assert(automatic.build(paragraphs, deterministic_measure));
    assert(automatic.pages == 2);
    assert(automatic.lines.size() == 4);
    assert(automatic.lines[2].page == 1);
    assert(automatic.lines[2].y == 5.0f);

    Document wrapped;
    PageSettings narrow_short = small_page(30, 40);
    narrow_short.top = narrow_short.bottom = 5;
    start_document(wrapped, narrow_short);
    const uint32_t long_text[] = {'a', 'b', 'c', 'd', 'e', 'f'};
    for (uint32_t cp : long_text) put_glyph(wrapped, cp);
    Layout wrapped_layout;
    assert(wrapped_layout.build(wrapped, deterministic_measure));
    assert(wrapped_layout.pages == 2);
    assert(wrapped_layout.lines.size() == 3);
    assert(wrapped_layout.lines[0].page == 0);
    assert(wrapped_layout.lines[2].page == 1 && wrapped_layout.lines[2].y == 5.0f);

    Document manual;
    start_document(manual, small_page());
    put_glyph(manual, 'A');
    put_glyph(manual, '\f');
    put_glyph(manual, 'B');
    Layout manual_layout;
    assert(manual_layout.build(manual, deterministic_measure));
    assert(manual_layout.pages == 2);
    assert(manual_layout.lines.size() == 2);
    assert(manual_layout.lines[0].page == 0 && manual_layout.lines[1].page == 1);
    assert(manual_layout.boxes[1].line == 0);
    assert(manual_layout.boxes[2].line == 1);
    assert(manual_layout.line_for(1) == 0 && manual_layout.line_for(2) == 1);

    Document terminal_break;
    start_document(terminal_break, small_page());
    put_glyph(terminal_break, 'A');
    put_glyph(terminal_break, '\f');
    Layout terminal_layout;
    assert(terminal_layout.build(terminal_break, deterministic_measure));
    assert(terminal_layout.pages == 2);
    assert(terminal_layout.lines.size() == 2);
    assert(terminal_layout.boxes[1].line == 0);
    assert(terminal_layout.boxes[2].line == 1);

    Document valid;
    start_document(valid, small_page());
    put_glyph(valid, 'A');
    Layout preserved;
    assert(preserved.build(valid, deterministic_measure));
    const size_t old_line_count = preserved.lines.size();
    const size_t old_box_count = preserved.boxes.size();
    const size_t old_boundary_count = preserved.grapheme_boundaries.size();
    const unsigned old_page_count = preserved.pages;
    Document invalid;
    PageSettings impossible = small_page(20);
    start_document(invalid, impossible);
    assert(!preserved.build(invalid, deterministic_measure));
    assert(preserved.lines.size() == old_line_count);
    assert(preserved.boxes.size() == old_box_count);
    assert(preserved.grapheme_boundaries.size() == old_boundary_count);
    assert(preserved.grapheme_boundaries[0] == 1);
    assert(preserved.grapheme_boundaries[1] == 1);
    assert(preserved.pages == old_page_count);
}

static void test_page_limit_and_many_paragraph_indexes() {
    Document too_many_pages;
    start_document(too_many_pages, small_page());
    for (unsigned i = 0; i < 4096; ++i) put_glyph(too_many_pages, '\f');
    Layout capped;
    assert(!capped.build(too_many_pages, deterministic_measure));

    PageSettings page = small_page(100, 40);
    page.top = page.bottom = 5;
    Document many;
    start_document(many, page);
    for (unsigned i = 0; i < 120; ++i) {
        put_glyph(many, 'x');
        if (i + 1 < 120) {
            put_glyph(many, '\n');
            put_paragraph(many, many.chars.size(), ParagraphStyle{});
        }
    }
    Layout layout;
    assert(layout.build(many, deterministic_measure));
    assert(layout.lines.size() == 120);
    assert(layout.pages > 1);
    for (size_t i = 0; i < layout.boxes.size(); ++i)
        assert(layout.boxes[i].line < layout.lines.size());
    assert(layout.line_for(0) == 0);
    assert(layout.line_for(100) > 0);
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "empty")) test_builds_empty_document_with_page_relative_caret();
        else if (!strcmp(argv[1], "one-line")) test_places_glyphs_and_nearest_carets_on_one_line();
        else if (!strcmp(argv[1], "wrapping")) test_wraps_latin_words_at_spaces_and_long_words_at_clusters();
        else if (!strcmp(argv[1], "unicode")) test_preserves_chinese_punctuation_combining_and_emoji_clusters();
        else if (!strcmp(argv[1], "hit-graphemes")) test_hit_test_never_places_caret_inside_grapheme_clusters();
        else if (!strcmp(argv[1], "glue-tabs")) test_nonbreaking_glue_and_tabs_keep_measured_positions();
        else if (!strcmp(argv[1], "metrics-align")) test_mixed_font_metrics_alignment_and_justification();
        else if (!strcmp(argv[1], "paragraphs")) test_paragraph_spacing_lists_and_trailing_empty_paragraph();
        else if (!strcmp(argv[1], "pages")) test_page_overflow_manual_breaks_and_atomic_rejection();
        else if (!strcmp(argv[1], "page-limit")) test_page_limit_and_many_paragraph_indexes();
        else assert(!"unknown test name");
        return 0;
    }
    test_builds_empty_document_with_page_relative_caret();
    test_places_glyphs_and_nearest_carets_on_one_line();
    test_wraps_latin_words_at_spaces_and_long_words_at_clusters();
    test_preserves_chinese_punctuation_combining_and_emoji_clusters();
    test_hit_test_never_places_caret_inside_grapheme_clusters();
    test_nonbreaking_glue_and_tabs_keep_measured_positions();
    test_mixed_font_metrics_alignment_and_justification();
    test_paragraph_spacing_lists_and_trailing_empty_paragraph();
    test_page_overflow_manual_breaks_and_atomic_rejection();
    test_page_limit_and_many_paragraph_indexes();
    return 0;
}
