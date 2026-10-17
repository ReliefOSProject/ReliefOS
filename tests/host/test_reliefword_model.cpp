#include "model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text_equals(const rw::Text &text, const char *expected) {
    return text.size() == strlen(expected) &&
        memcmp(text.c_str(), expected, text.size()) == 0;
}

static bool page_equal(const rw::PageSettings &a, const rw::PageSettings &b) {
    return a.width == b.width && a.height == b.height && a.left == b.left &&
        a.right == b.right && a.top == b.top && a.bottom == b.bottom &&
        a.page_numbers == b.page_numbers && a.header == b.header && a.footer == b.footer;
}

static void expect_document_text(const rw::Document &doc, const char *expected) {
    rw::Text text;
    assert(rw::document_text(doc, 0, doc.chars.size(), text));
    assert(text_equals(text, expected));
}

static void test_utf8_scalar_validation_and_encoding() {
    assert(rw::valid_scalar(0));
    assert(rw::valid_scalar(0x10ffff));
    assert(!rw::valid_scalar(0xd800));
    assert(!rw::valid_scalar(0x110000));
    assert(rw::valid_utf8("中文🙂", strlen("中文🙂")));

    const char overlong[] = {char(0xc0), char(0xaf)};
    const char surrogate[] = {char(0xed), char(0xa0), char(0x80)};
    const char too_large[] = {char(0xf4), char(0x90), char(0x80), char(0x80)};
    const char truncated[] = {char(0xe4), char(0xb8)};
    const char stray[] = {char(0x80)};
    assert(!rw::valid_utf8(overlong, sizeof(overlong)));
    assert(!rw::valid_utf8(surrogate, sizeof(surrogate)));
    assert(!rw::valid_utf8(too_large, sizeof(too_large)));
    assert(!rw::valid_utf8(truncated, sizeof(truncated)));
    assert(!rw::valid_utf8(stray, sizeof(stray)));
    assert(!rw::valid_utf8(nullptr, 1));

    rw::Array<uint32_t> decoded;
    rw::Text error;
    assert(rw::decode_utf8("A中🙂", strlen("A中🙂"), decoded, error));
    assert(decoded.size() == 3);
    assert(decoded[0] == 'A' && decoded[1] == 0x4e2d && decoded[2] == 0x1f642);
    const char invalid[] = {char(0xf0), char(0x80), char(0x80), char(0x80)};
    const size_t old_size = decoded.size();
    assert(!rw::decode_utf8(invalid, sizeof(invalid), decoded, error));
    assert(decoded.size() == old_size);
    char bytes[4] = {};
    assert(rw::encode_utf8(0x1f642, bytes) == 4);
    assert(memcmp(bytes, "🙂", 4) == 0);
    assert(rw::encode_utf8(0xd800, bytes) == 0);
}

static void test_document_invariants_text_and_default_page() {
    rw::Document doc;
    assert(doc.init());
    assert(doc.chars.empty());
    assert(doc.styles.size() == 1);
    assert(doc.paragraphs.size() == 1 && doc.paragraphs[0].start == 0);
    assert(doc.page.width == 595 && doc.page.height == 842);
    rw::Text error;
    assert(doc.validate(error));

    rw::Editor editor;
    assert(editor.insert_utf8("中\r\n文\r尾", strlen("中\r\n文\r尾")));
    expect_document_text(editor.doc, "中\n文\n尾");
    assert(editor.doc.paragraphs.size() == 3);
    assert(editor.doc.paragraphs[0].start == 0);
    assert(editor.doc.paragraphs[1].start == 2);
    assert(editor.doc.paragraphs[2].start == 4);
    assert(editor.doc.validate(error));

    rw::Editor page_break;
    assert(page_break.insert_utf8("a\fb", 3));
    assert(page_break.doc.chars.size() == 3);
    assert(page_break.doc.paragraphs.size() == 1);
    assert(page_break.doc.chars[1].codepoint == '\f');
    assert(page_break.doc.validate(error));

    rw::Text slice;
    assert(rw::document_text(editor.doc, 2, 5, slice));
    assert(text_equals(slice, "文\n尾"));
    assert(!rw::document_text(editor.doc, 0, editor.doc.chars.size() + 1, slice));

    rw::Document invalid;
    assert(invalid.init());
    invalid.chars.resize(1);
    invalid.chars[0] = {'x', 2};
    assert(!invalid.validate(error));
    invalid.chars.clear();
    invalid.styles[0].size = 0;
    assert(!invalid.validate(error));
}

static void test_statistics_count_mixed_chinese_latin_and_paragraphs() {
    rw::Editor editor;
    assert(editor.insert_utf8("你好 world!\nABC", strlen("你好 world!\nABC")));
    const rw::Statistics all = rw::statistics(editor.doc);
    assert(all.characters == 13);
    assert(all.nonspace == 11);
    assert(all.words == 4);
    assert(all.paragraphs == 2);
    const rw::Statistics selection = rw::statistics(editor.doc, 3, 9);
    assert(selection.characters == 6);
    assert(selection.words == 1);
    assert(selection.paragraphs == 1);
}

static void test_cluster_movement_and_backspace_keep_grapheme_subset() {
    rw::Editor editor;
    const char sample[] = "A\xcc\x81🙂\xef\xb8\x8f👍🏽👩‍👩‍👧‍👦🇨🇳Z";
    assert(editor.insert_utf8(sample, strlen(sample)));
    const size_t end = editor.doc.chars.size();
    const size_t z = end - 1;
    assert(rw::next_cluster(editor.doc, 0) == 2);
    assert(rw::next_cluster(editor.doc, 2) == 4);
    assert(rw::next_cluster(editor.doc, 4) == 6);
    assert(rw::next_cluster(editor.doc, 6) == 13);
    assert(rw::next_cluster(editor.doc, 13) == 15);
    assert(rw::next_cluster(editor.doc, 15) == 16);
    assert(rw::previous_cluster(editor.doc, end) == z);
    const size_t flag = rw::previous_cluster(editor.doc, z);
    assert(flag + 2 == z);
    const size_t family = rw::previous_cluster(editor.doc, flag);
    assert(editor.doc.chars[family].codepoint == 0x1f469);
    const size_t modifier = rw::previous_cluster(editor.doc, family);
    assert(editor.doc.chars[modifier].codepoint == 0x1f44d);
    const size_t variation = rw::previous_cluster(editor.doc, modifier);
    assert(editor.doc.chars[variation].codepoint == 0x1f642);
    const size_t combining = rw::previous_cluster(editor.doc, variation);
    assert(combining == 0);

    editor.select(end, end);
    const size_t old_anchor = editor.anchor, old_caret = editor.caret;
    assert(editor.erase(true));
    expect_document_text(editor.doc, "A\xcc\x81🙂\xef\xb8\x8f👍🏽👩‍👩‍👧‍👦🇨🇳");
    assert(editor.caret == end - 1);
    assert(editor.undo());
    expect_document_text(editor.doc, sample);
    assert(editor.anchor == old_anchor && editor.caret == old_caret);
}

static void test_selection_replace_preserves_paragraph_inheritance_and_history() {
    rw::Editor editor;
    assert(editor.insert_utf8("alpha\nbeta\ngamma", strlen("alpha\nbeta\ngamma")));

    rw::ParagraphStyle first;
    first.alignment = rw::Alignment::Center;
    first.left_indent = 18;
    editor.select(0, 5);
    assert(editor.apply_paragraph(first));
    rw::ParagraphStyle second;
    second.alignment = rw::Alignment::Right;
    second.after = 14;
    editor.select(6, 10);
    assert(editor.apply_paragraph(second));

    editor.select(2, 8);
    const size_t prior_anchor = editor.anchor;
    const size_t prior_caret = editor.caret;
    const uint64_t prior_revision = editor.doc.revision;
    assert(editor.insert_utf8("X\nY", 3));
    expect_document_text(editor.doc, "alX\nYta\ngamma");
    assert(editor.caret == 5 && editor.anchor == 5);
    assert(editor.doc.paragraphs.size() == 3);
    assert(editor.doc.paragraphs[0].style == first);
    assert(editor.doc.paragraphs[1].style == second);
    assert(editor.doc.paragraphs[2].style == rw::ParagraphStyle{});
    const uint64_t edited_revision = editor.doc.revision;
    assert(edited_revision > prior_revision);
    assert(editor.undo());
    expect_document_text(editor.doc, "alpha\nbeta\ngamma");
    assert(editor.anchor == prior_anchor && editor.caret == prior_caret);
    assert(editor.doc.revision == prior_revision);
    assert(editor.can_redo());
    assert(editor.redo());
    expect_document_text(editor.doc, "alX\nYta\ngamma");
    assert(editor.caret == 5 && editor.anchor == 5);
    assert(editor.doc.revision == edited_revision);
}

static void test_rich_copy_paste_remaps_styles_and_paragraphs() {
    rw::Editor source;
    assert(source.insert_utf8("ab\ncd", strlen("ab\ncd")));
    rw::ParagraphStyle first;
    first.alignment = rw::Alignment::Center;
    source.select(0, 2);
    assert(source.apply_paragraph(first));
    rw::ParagraphStyle second;
    second.alignment = rw::Alignment::Right;
    source.select(3, 5);
    assert(source.apply_paragraph(second));
    rw::TextStyle emphasis;
    emphasis.bold = 1;
    emphasis.foreground = 0x123456;
    source.select(1, 4);
    assert(source.apply_style(emphasis, rw::Bold | rw::Foreground));

    source.select(1, 4);
    rw::Document fragment;
    assert(source.copy_selection(fragment));
    expect_document_text(fragment, "b\nc");
    assert(fragment.paragraphs.size() == 2);
    assert(fragment.paragraphs[0].style == first);
    assert(fragment.paragraphs[1].style == second);
    assert(fragment.chars[0].style < fragment.styles.size());
    assert(fragment.styles[fragment.chars[0].style].bold == 1);
    assert(fragment.styles[fragment.chars[2].style].foreground == 0x123456);

    rw::Editor target;
    assert(target.insert_utf8("x", 1));
    target.select(1, 1);
    assert(target.paste_document(fragment));
    expect_document_text(target.doc, "xb\nc");
    assert(target.doc.paragraphs.size() == 2);
    assert(target.doc.paragraphs[0].style == first);
    assert(target.doc.paragraphs[1].style == second);
    const uint16_t pasted_style = target.doc.chars[1].style;
    assert(target.doc.styles[pasted_style].bold == 1);
    assert(target.doc.styles[pasted_style].foreground == 0x123456);
    assert(target.undo());
    expect_document_text(target.doc, "x");
    assert(target.doc.paragraphs[0].style == rw::ParagraphStyle{});
    assert(target.redo());
    expect_document_text(target.doc, "xb\nc");
}

static void test_style_masks_typing_style_and_paragraph_updates() {
    rw::Editor editor;
    assert(editor.insert_utf8("word\nnext", strlen("word\nnext")));
    rw::TextStyle style;
    style.bold = 1;
    style.italic = 1;
    style.foreground = 0xabcdef;
    editor.select(0, 4);
    assert(editor.apply_style(style, rw::Bold | rw::Foreground));
    assert(editor.doc.styles[editor.doc.chars[0].style].bold == 1);
    assert(editor.doc.styles[editor.doc.chars[0].style].italic == 0);
    assert(editor.doc.styles[editor.doc.chars[0].style].foreground == 0xabcdef);

    rw::ParagraphStyle para;
    para.alignment = rw::Alignment::Justify;
    para.list = rw::ListKind::Number;
    para.first_indent = 12;
    para.before = 8;
    editor.select(5, 9);
    assert(editor.apply_paragraph(para));
    assert(editor.doc.paragraphs[0].style.alignment == rw::Alignment::Left);
    assert(editor.doc.paragraphs[1].style == para);

    editor.select(4, 4);
    rw::TextStyle typing;
    typing.underline = 1;
    assert(editor.apply_style(typing, rw::Underline));
    const uint64_t before_insert = editor.doc.revision;
    assert(editor.insert_utf8("!", 1));
    assert(editor.doc.revision > before_insert);
    assert(editor.doc.styles[editor.doc.chars[4].style].underline == 1);
}

static void test_paragraph_mask_preserves_unselected_properties() {
    rw::Editor editor;
    assert(editor.insert_utf8("first\nsecond", strlen("first\nsecond")));
    rw::ParagraphStyle first;
    first.left_indent = 14;
    first.after = 20;
    editor.select(0, 5);
    assert(editor.apply_paragraph(first));
    rw::ParagraphStyle second;
    second.right_indent = 26;
    second.before = 12;
    editor.select(6, 12);
    assert(editor.apply_paragraph(second));

    rw::ParagraphStyle alignment;
    alignment.alignment = rw::Alignment::Center;
    alignment.left_indent = 4000; // Ignored by the alignment-only mask.
    alignment.line_spacing = 0;
    editor.select(0, editor.doc.chars.size());
    assert(editor.apply_paragraph(alignment, rw::ParaAlignment));
    assert(editor.doc.paragraphs[0].style.alignment == rw::Alignment::Center);
    assert(editor.doc.paragraphs[0].style.left_indent == 14);
    assert(editor.doc.paragraphs[0].style.after == 20);
    assert(editor.doc.paragraphs[1].style.alignment == rw::Alignment::Center);
    assert(editor.doc.paragraphs[1].style.right_indent == 26);
    assert(editor.doc.paragraphs[1].style.before == 12);
}

static void test_page_settings_are_validated_and_undoable() {
    rw::Editor editor;
    rw::PageSettings page = editor.doc.page;
    page.width = 612;
    page.height = 792;
    page.left = 48;
    page.right = 48;
    assert(page.header.set("机密", strlen("机密")));
    assert(editor.set_page(page));
    assert(editor.doc.page.width == 612 && editor.doc.page.header == "机密");
    const uint64_t revision = editor.doc.revision;
    rw::PageSettings invalid = page;
    invalid.left = invalid.width;
    assert(!editor.set_page(invalid));
    assert(editor.doc.revision == revision);
    assert(page_equal(editor.doc.page, page));
    assert(editor.undo());
    assert(editor.doc.page.width == 595);
    assert(editor.redo());
    assert(page_equal(editor.doc.page, page));
}

static void test_find_and_replace_all_are_case_aware_and_one_undo() {
    rw::Editor editor;
    assert(editor.insert_utf8("Cat cat CAT 中cat", strlen("Cat cat CAT 中cat")));
    size_t start = 99, end = 100;
    assert(editor.find("cat", false, 1, start, end));
    assert(start == 4 && end == 7);
    assert(editor.find("cat", true, 1, start, end));
    assert(start == 4 && end == 7);
    assert(!editor.find("missing", false, 0, start, end));
    assert(start == 4 && end == 7);

    editor.select(2, 5);
    const size_t old_anchor = editor.anchor, old_caret = editor.caret;
    unsigned count = 0;
    assert(editor.replace_all("cat", "犬\n", false, count));
    assert(count == 4);
    expect_document_text(editor.doc, "犬\n 犬\n 犬\n 中犬\n");
    assert(editor.doc.paragraphs.size() == 5);
    assert(editor.anchor == editor.caret);
    assert(editor.undo());
    expect_document_text(editor.doc, "Cat cat CAT 中cat");
    assert(editor.anchor == old_anchor && editor.caret == old_caret);
    assert(editor.redo());
    expect_document_text(editor.doc, "犬\n 犬\n 犬\n 中犬\n");

    const uint64_t revision = editor.doc.revision;
    assert(!editor.replace_all("", "x", false, count));
    assert(editor.doc.revision == revision);
}

static void test_replace_all_preserves_trailing_paragraph_style() {
    rw::Editor editor;
    assert(editor.insert_utf8("a\nb", strlen("a\nb")));
    rw::ParagraphStyle first;
    first.alignment = rw::Alignment::Center;
    editor.select(0, 1);
    assert(editor.apply_paragraph(first));
    rw::ParagraphStyle second;
    second.alignment = rw::Alignment::Right;
    editor.select(2, 3);
    assert(editor.apply_paragraph(second));
    editor.select(0, 2);
    const size_t old_anchor = editor.anchor, old_caret = editor.caret;

    unsigned count = 0;
    assert(editor.replace_all("\n", "\n\n", true, count));
    assert(count == 1);
    expect_document_text(editor.doc, "a\n\nb");
    assert(editor.doc.paragraphs.size() == 3);
    assert(editor.doc.paragraphs[0].style == first);
    assert(editor.doc.paragraphs[1].style == first);
    assert(editor.doc.paragraphs[2].style == second);
    assert(editor.undo());
    expect_document_text(editor.doc, "a\nb");
    assert(editor.anchor == old_anchor && editor.caret == old_caret);
}

static void test_large_bulk_replace_builds_one_candidate() {
    const size_t length = 100000;
    char *source = static_cast<char *>(malloc(length));
    assert(source);
    memset(source, 'a', length);
    rw::Editor editor;
    assert(editor.insert_utf8(source, length));
    free(source);
    unsigned count = 0;
    assert(editor.replace_all("aa", "b", true, count));
    assert(count == length / 2);
    assert(editor.doc.chars.size() == length / 2);
    assert(editor.doc.chars[0].codepoint == 'b');
    assert(editor.doc.chars[length / 2 - 1].codepoint == 'b');
    assert(editor.undo());
    assert(editor.doc.chars.size() == length);
    assert(editor.doc.chars[0].codepoint == 'a');
    assert(editor.redo());
    assert(editor.doc.chars.size() == length / 2);
}

static void test_history_trims_old_snapshots_to_its_budget() {
    const size_t length = rw::MaxChars;
    char *source = static_cast<char *>(malloc(length));
    assert(source);
    memset(source, 'x', length);
    rw::Editor editor;
    assert(editor.insert_utf8(source, length));
    free(source);
    editor.select(0, 1);
    for (uint32_t n = 0; n < 12; ++n) {
        rw::TextStyle style;
        style.foreground = 0x100000 + n;
        assert(editor.apply_style(style, rw::Foreground));
    }
    unsigned undos = 0;
    while (editor.undo()) ++undos;
    assert(undos > 0 && undos < 13);
    assert(editor.doc.chars.size() == length);
    assert(editor.can_redo());
}

static void test_rejections_preserve_document_selection_and_history() {
    rw::Editor editor;
    assert(editor.insert_utf8("稳定", strlen("稳定")));
    editor.select(0, 1);
    const uint64_t revision = editor.doc.revision;
    const size_t anchor = editor.anchor, caret = editor.caret;
    const size_t chars = editor.doc.chars.size();
    const bool could_undo = editor.can_undo();
    const char bad[] = {char(0xf0), char(0x28), char(0x8c), char(0x28)};
    assert(!editor.insert_utf8(bad, sizeof(bad)));
    assert(editor.doc.revision == revision);
    assert(editor.anchor == anchor && editor.caret == caret);
    assert(editor.doc.chars.size() == chars && editor.can_undo() == could_undo);
    expect_document_text(editor.doc, "稳定");

    char *too_many = static_cast<char *>(malloc(rw::MaxChars + 1));
    assert(too_many);
    memset(too_many, 'x', rw::MaxChars + 1);
    editor.select(chars, chars);
    const size_t old_anchor = editor.anchor, old_caret = editor.caret;
    assert(!editor.insert_utf8(too_many, rw::MaxChars + 1));
    free(too_many);
    assert(editor.doc.revision == revision);
    assert(editor.anchor == old_anchor && editor.caret == old_caret);
    assert(editor.doc.chars.size() == chars);

    char *too_many_paragraphs = static_cast<char *>(malloc(rw::MaxParagraphs));
    assert(too_many_paragraphs);
    memset(too_many_paragraphs, '\n', rw::MaxParagraphs);
    assert(!editor.insert_utf8(too_many_paragraphs, rw::MaxParagraphs));
    free(too_many_paragraphs);
    assert(editor.doc.revision == revision);
    assert(editor.doc.chars.size() == chars);
    assert(editor.anchor == old_anchor && editor.caret == old_caret);

    rw::ParagraphStyle bad_para;
    bad_para.alignment = static_cast<rw::Alignment>(255);
    editor.select(0, 1);
    const size_t stable_anchor = editor.anchor, stable_caret = editor.caret;
    assert(!editor.apply_paragraph(bad_para));
    assert(editor.doc.revision == revision);
    assert(editor.anchor == stable_anchor && editor.caret == stable_caret);

    assert(editor.undo());
    expect_document_text(editor.doc, "");
    const bool had_redo = editor.can_redo();
    const bool had_undo = editor.can_undo();
    assert(had_redo && !had_undo);
    assert(!editor.insert_utf8(bad, sizeof(bad)));
    assert(editor.can_redo() == had_redo && editor.can_undo() == had_undo);
    expect_document_text(editor.doc, "");
}

static void test_style_table_limit_rejects_without_partial_change() {
    rw::Editor editor;
    assert(editor.insert_utf8("x", 1));
    editor.select(0, 1);
    for (unsigned n = 1; n < rw::MaxStyles; ++n) {
        rw::TextStyle style;
        style.foreground = n;
        assert(editor.apply_style(style, rw::Foreground));
    }
    assert(editor.doc.styles.size() == rw::MaxStyles);
    const uint64_t revision = editor.doc.revision;
    const uint16_t old_style = editor.doc.chars[0].style;
    rw::TextStyle overflow;
    overflow.foreground = 0xabcdef;
    assert(!editor.apply_style(overflow, rw::Foreground));
    assert(editor.doc.revision == revision);
    assert(editor.doc.styles.size() == rw::MaxStyles);
    assert(editor.doc.chars[0].style == old_style);
    assert(editor.can_undo());
}

static void test_replace_document_validates_before_swapping() {
    rw::Editor editor;
    assert(editor.insert_utf8("old", 3));
    rw::Document replacement;
    assert(replacement.init());
    rw::Glyph invalid{'x', 4};
    assert(replacement.chars.push(invalid));
    const uint64_t revision = editor.doc.revision;
    const bool had_undo = editor.can_undo(), had_redo = editor.can_redo();
    assert(!editor.replace_document(static_cast<rw::Document &&>(replacement)));
    expect_document_text(editor.doc, "old");
    assert(editor.doc.revision == revision);
    assert(editor.can_undo() == had_undo && editor.can_redo() == had_redo);

    assert(editor.undo());
    expect_document_text(editor.doc, "");
    assert(!editor.can_undo() && editor.can_redo());

    rw::Document good;
    assert(good.init());
    rw::Editor donor;
    assert(donor.insert_utf8("新文档", strlen("新文档")));
    assert(good.clone_from(donor.doc));
    assert(editor.replace_document(static_cast<rw::Document &&>(good)));
    expect_document_text(editor.doc, "新文档");
    const uint64_t replacement_revision = editor.doc.revision;
    assert(replacement_revision > revision);
    assert(!editor.can_undo() && !editor.can_redo());
    assert(editor.anchor == 0 && editor.caret == 0);
    assert(editor.insert_utf8("!", 1));
    assert(editor.doc.revision == replacement_revision + 1);
    expect_document_text(editor.doc, "!新文档");
}

static void test_revision_ids_remain_unique_across_history_branches_and_commit_paths() {
    rw::Editor editor;
    assert(editor.insert_utf8("A", 1));
    const uint64_t initial_revision = editor.doc.revision;
    assert(editor.insert_utf8("X", 1));
    const uint64_t saved_revision = editor.doc.revision;
    assert(saved_revision > initial_revision);

    assert(editor.undo());
    assert(editor.doc.revision == initial_revision);
    assert(editor.redo());
    assert(editor.doc.revision == saved_revision);
    assert(editor.undo());
    assert(editor.doc.revision == initial_revision);

    assert(editor.insert_utf8("B", 1));
    uint64_t highest_revision = editor.doc.revision;
    assert(highest_revision > saved_revision);
    expect_document_text(editor.doc, "AB");
    assert(editor.undo());
    assert(editor.doc.revision == initial_revision);
    expect_document_text(editor.doc, "A");

    auto record_fresh_revision = [&]() {
        assert(editor.doc.revision > highest_revision);
        highest_revision = editor.doc.revision;
    };
    auto undo_to_initial = [&]() {
        assert(editor.undo());
        assert(editor.doc.revision == initial_revision);
        expect_document_text(editor.doc, "A");
    };

    editor.select(0, 1);
    rw::TextStyle bold;
    bold.bold = 1;
    assert(editor.apply_style(bold, rw::Bold));
    record_fresh_revision();
    undo_to_initial();

    editor.select(0, 1);
    rw::ParagraphStyle centered;
    centered.alignment = rw::Alignment::Center;
    assert(editor.apply_paragraph(centered, rw::ParaAlignment));
    record_fresh_revision();
    undo_to_initial();

    rw::PageSettings page = editor.doc.page;
    page.width = 612;
    page.height = 792;
    assert(editor.set_page(page));
    record_fresh_revision();
    undo_to_initial();

    rw::Editor donor;
    assert(donor.insert_utf8("Z", 1));
    donor.select(0, 1);
    rw::Document fragment;
    assert(donor.copy_selection(fragment));
    editor.select(1, 1);
    assert(editor.paste_document(fragment));
    record_fresh_revision();
    expect_document_text(editor.doc, "AZ");
    undo_to_initial();

    unsigned replacements = 0;
    assert(editor.replace_all("A", "C", true, replacements));
    assert(replacements == 1);
    record_fresh_revision();
    expect_document_text(editor.doc, "C");
    undo_to_initial();

    editor.select(1, 1);
    editor.reset_history();
    assert(!editor.can_undo() && !editor.can_redo());
    assert(editor.insert_utf8("D", 1));
    record_fresh_revision();
    expect_document_text(editor.doc, "AD");

    rw::Document replacement;
    assert(replacement.init());
    assert(replacement.chars.push({'R', 0}));
    assert(editor.replace_document(static_cast<rw::Document &&>(replacement)));
    record_fresh_revision();
    const uint64_t replacement_revision = editor.doc.revision;
    expect_document_text(editor.doc, "R");
    assert(!editor.can_undo() && !editor.can_redo());
    assert(editor.insert_utf8("!", 1));
    assert(editor.doc.revision == replacement_revision + 1);
    highest_revision = editor.doc.revision;
    expect_document_text(editor.doc, "!R");
    assert(editor.undo());
    assert(editor.doc.revision == replacement_revision);
    expect_document_text(editor.doc, "R");
    assert(editor.redo());
    assert(editor.doc.revision == highest_revision);
    expect_document_text(editor.doc, "!R");
    assert(editor.undo());
    assert(editor.doc.revision == replacement_revision);
    expect_document_text(editor.doc, "R");

    rw::Document invalid;
    assert(invalid.init());
    assert(invalid.chars.push({'?', 1}));
    const uint64_t before_rejection = editor.doc.revision;
    const bool had_undo = editor.can_undo(), had_redo = editor.can_redo();
    assert(!editor.replace_document(static_cast<rw::Document &&>(invalid)));
    assert(editor.doc.revision == before_rejection);
    assert(editor.can_undo() == had_undo && editor.can_redo() == had_redo);
    expect_document_text(editor.doc, "R");
    assert(editor.insert_utf8("?", 1));
    assert(editor.doc.revision == highest_revision + 1);
    expect_document_text(editor.doc, "?R");
}

static void test_revision_exhaustion_does_not_change_history_or_high_water() {
    rw::Editor editor;
    assert(editor.insert_utf8("A", 1));
    const uint64_t initial_revision = editor.doc.revision;
    assert(editor.insert_utf8("X", 1));
    const uint64_t highest_revision = editor.doc.revision;
    const size_t anchor = editor.anchor, caret = editor.caret;
    const bool had_undo = editor.can_undo(), had_redo = editor.can_redo();

    editor.doc.revision = UINT64_MAX;
    assert(!editor.insert_utf8("Q", 1));
    assert(text_equals(editor.error, "文档版本号已达上限"));
    assert(editor.doc.revision == UINT64_MAX);
    assert(editor.anchor == anchor && editor.caret == caret);
    assert(editor.can_undo() == had_undo && editor.can_redo() == had_redo);
    expect_document_text(editor.doc, "AX");

    rw::Document replacement;
    assert(replacement.init());
    assert(replacement.chars.push({'R', 0}));
    assert(!editor.replace_document(static_cast<rw::Document &&>(replacement)));
    assert(text_equals(editor.error, "文档版本号已达上限"));
    assert(editor.doc.revision == UINT64_MAX);
    assert(editor.anchor == anchor && editor.caret == caret);
    assert(editor.can_undo() == had_undo && editor.can_redo() == had_redo);
    expect_document_text(editor.doc, "AX");

    assert(editor.undo());
    assert(editor.doc.revision == initial_revision);
    expect_document_text(editor.doc, "A");
    assert(editor.insert_utf8("B", 1));
    assert(editor.doc.revision == highest_revision + 1);
    expect_document_text(editor.doc, "AB");
}

int main() {
    test_utf8_scalar_validation_and_encoding();
    test_document_invariants_text_and_default_page();
    test_statistics_count_mixed_chinese_latin_and_paragraphs();
    test_cluster_movement_and_backspace_keep_grapheme_subset();
    test_selection_replace_preserves_paragraph_inheritance_and_history();
    test_rich_copy_paste_remaps_styles_and_paragraphs();
    test_style_masks_typing_style_and_paragraph_updates();
    test_paragraph_mask_preserves_unselected_properties();
    test_page_settings_are_validated_and_undoable();
    test_find_and_replace_all_are_case_aware_and_one_undo();
    test_replace_all_preserves_trailing_paragraph_style();
    test_large_bulk_replace_builds_one_candidate();
    test_history_trims_old_snapshots_to_its_budget();
    test_rejections_preserve_document_selection_and_history();
    test_style_table_limit_rejects_without_partial_change();
    test_replace_document_validates_before_swapping();
    test_revision_ids_remain_unique_across_history_branches_and_commit_paths();
    test_revision_exhaustion_does_not_change_history_or_high_water();
    puts("ok - ReliefWord model behaviors");
    return 0;
}
