#include "formula.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_utf8_and_addresses() {
    assert(rx::valid_utf8("记账", strlen("记账")));
    const char overlong[] = {char(0xc0), char(0xaf)};
    const char surrogate[] = {char(0xed), char(0xa0), char(0x80)};
    const char truncated[] = {char(0xe4), char(0xb8)};
    assert(!rx::valid_utf8(overlong, sizeof(overlong)));
    assert(!rx::valid_utf8(surrogate, sizeof(surrogate)));
    assert(!rx::valid_utf8(truncated, sizeof(truncated)));
    assert(rx::valid_sheet_name("2026年预算"));
    assert(!rx::valid_sheet_name("bad/name"));

    int row = -1, col = -1;
    assert(rx::parse_address("AA10", row, col));
    assert(row == 9 && col == 26);
    assert(!rx::parse_address("A0", row, col));
    assert(!rx::parse_address("XFE1", row, col));
}

static bool cancel_progress(void *, unsigned, unsigned);

static void test_sparse_model_clone_and_formatting() {
    rx::Workbook source;
    assert(source.init());
    const uint32_t id = source.active_id;
    assert(source.set_input(id, 4, 2, "12.5"));
    rx::Style style;
    style.bold = 1;
    style.background = 0xffe080;
    style.format = rx::NumberFormat::Currency;
    assert(source.sheet(id)->set_style(4, 2, style));
    assert(source.cell_count() == 1);
    assert(source.sheet(id)->used_rows() == 5);
    assert(source.sheet(id)->used_columns() == 3);

    rx::Workbook copy;
    assert(copy.clone_from(source));
    assert(copy.get(id, 4, 2)->style == style);
    assert(copy.get(id, 4, 2)->input == "12.5");
    assert(source.set_input(id, 4, 2, "changed"));
    assert(copy.get(id, 4, 2)->input == "12.5");
    assert(copy.set_input(id, 4, 2, ""));
    assert(copy.get(id, 4, 2)->input.empty());
    assert(copy.get(id, 4, 2)->style == style);

    rx::Text rendered = rx::format_value(rx::Value::string("中文"));
    assert(rendered == "中文");
    rendered = rx::format_value(rx::Value::numeric(12.5), style);
    assert(rendered == "¥12.50");
}

static void test_set_cell_history_and_rejection_are_atomic() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "原始"));
    assert(editor.book.get(id, 0, 0)->input == "原始");
    assert(editor.can_undo());
    assert(editor.undo());
    assert(editor.book.get(id, 0, 0) == nullptr);
    assert(editor.can_redo());
    assert(editor.redo());
    assert(editor.book.get(id, 0, 0)->input == "原始");
    const uint64_t stable_revision = editor.book.revision;

    char oversized[rx::MaxTextBytes + 2];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = 0;
    const char malformed[] = {char(0xf0), char(0x80), char(0x80), char(0x80), 0};
    assert(!editor.set_cell(id, 1, 0, oversized));
    assert(!editor.set_cell(id, 1, 0, malformed));
    assert(!editor.set_cell(id, rx::MaxRows, 0, "outside"));
    assert(editor.book.revision == stable_revision);
    assert(editor.book.get(id, 1, 0) == nullptr);
    assert(editor.book.get(id, 0, 0)->input == "原始");
}

static void test_copy_paste_fill_and_clear_restore_style() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    rx::Style style;
    style.italic = 1;
    assert(editor.set_cell(id, 0, 0, "=$B$1+B1"));
    assert(editor.book.sheet(id)->set_style(0, 0, style));
    rx::Clipboard clipboard;
    assert(editor.copy_range(id, {0, 0, 0, 0}, clipboard));
    assert(editor.paste(id, 1, 0, clipboard));
    assert(editor.book.get(id, 1, 0)->input == "=$B$1+B2");
    assert(editor.book.get(id, 1, 0)->style == style);

    assert(editor.fill(id, {0, 0, 0, 0}, {0, 0, 3, 0}));
    assert(editor.book.get(id, 3, 0)->input == "=$B$1+B4");
    assert(editor.book.get(id, 3, 0)->style == style);
    assert(editor.clear_range(id, {0, 0, 3, 0}));
    assert(editor.book.get(id, 0, 0)->input.empty());
    assert(editor.book.get(id, 0, 0)->style == style);
    assert(editor.undo());
    assert(editor.book.get(id, 0, 0)->input == "=$B$1+B1");
    assert(editor.book.get(id, 0, 0)->style == style);
    assert(editor.redo());
    assert(editor.book.get(id, 0, 0)->input.empty());
    assert(editor.style_range(id, {0, 0, 3, 0}, rx::Style{}));
    assert(editor.book.get(id, 0, 0) == nullptr);
}

static void test_cut_paste_preserves_moved_formula_references() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "=B1+$C$1"));
    rx::Clipboard clipboard;
    assert(editor.copy_range(id, {0, 0, 0, 0}, clipboard, true));
    assert(editor.paste(id, 1, 0, clipboard));
    assert(editor.book.get(id, 0, 0) == nullptr);
    assert(editor.book.get(id, 1, 0)->input == "=B1+$C$1");
    assert(!clipboard.cut);
    assert(editor.undo());
    assert(editor.book.get(id, 0, 0)->input == "=B1+$C$1");
    assert(editor.book.get(id, 1, 0) == nullptr);
    assert(editor.redo());
    assert(editor.book.get(id, 1, 0)->input == "=B1+$C$1");
}

static void test_cut_paste_between_sheets_is_undoable() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t source = editor.book.active_id;
    assert(editor.add_sheet("Target"));
    const uint32_t target = editor.book.active_id;
    assert(editor.set_cell(source, 0, 0, "=B1+'Sheet1'!C$2"));

    rx::Clipboard clipboard;
    assert(editor.copy_range(source, {0, 0, 0, 0}, clipboard, true));
    assert(editor.paste(target, 2, 3, clipboard));
    assert(editor.book.get(source, 0, 0) == nullptr);
    assert(editor.book.get(target, 2, 3)->input == "='Sheet1'!B1+'Sheet1'!C$2");
    assert(!clipboard.cut);
    assert(editor.undo());
    assert(editor.book.get(source, 0, 0)->input == "=B1+'Sheet1'!C$2");
    assert(editor.book.get(target, 2, 3) == nullptr);
    assert(editor.redo());
    assert(editor.book.get(target, 2, 3)->input == "='Sheet1'!B1+'Sheet1'!C$2");
}

static void test_cut_retargets_same_sheet_dependents_and_moved_formulas() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "7"));
    assert(editor.set_cell(id, 0, 1, "=A1"));
    assert(editor.set_cell(id, 0, 2, "=\"A1\"&A1"));
    rx::Clipboard clipboard;
    assert(editor.copy_range(id, {0, 0, 0, 1}, clipboard, true));
    assert(editor.paste(id, 1, 0, clipboard));
    assert(editor.book.get(id, 1, 1)->input == "=A2");
    assert(editor.book.get(id, 0, 2)->input == "=\"A1\"&A2");
    assert(editor.undo());
    assert(editor.book.get(id, 0, 1)->input == "=A1");
    assert(editor.book.get(id, 0, 2)->input == "=\"A1\"&A1");
    assert(editor.book.get(id, 1, 0) == nullptr);
}

static void test_overlapping_cut_retargets_formulas_and_undo_redo() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "1"));
    assert(editor.set_cell(id, 1, 0, "=A1"));
    assert(editor.set_cell(id, 0, 2, "=SUM(A1:A2)"));

    rx::Clipboard clipboard;
    assert(editor.copy_range(id, {0, 0, 1, 0}, clipboard, true));
    assert(editor.paste(id, 1, 0, clipboard));
    assert(editor.book.get(id, 0, 0) == nullptr);
    assert(editor.book.get(id, 1, 0)->input == "1");
    assert(editor.book.get(id, 2, 0)->input == "=A2");
    assert(editor.book.get(id, 0, 2)->input == "=SUM(A2:A3)");
    assert(rx::evaluate(editor.book, id, 0, 2).number == 2.0);

    assert(editor.undo());
    assert(editor.book.get(id, 0, 0)->input == "1");
    assert(editor.book.get(id, 1, 0)->input == "=A1");
    assert(editor.book.get(id, 2, 0) == nullptr);
    assert(editor.book.get(id, 0, 2)->input == "=SUM(A1:A2)");
    assert(editor.redo());
    assert(editor.book.get(id, 0, 0) == nullptr);
    assert(editor.book.get(id, 1, 0)->input == "1");
    assert(editor.book.get(id, 2, 0)->input == "=A2");
    assert(editor.book.get(id, 0, 2)->input == "=SUM(A2:A3)");
}

static void test_cut_retargets_cross_sheet_references_and_preserves_moved_context() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t source = editor.book.active_id;
    assert(editor.add_sheet("Target"));
    const uint32_t target = editor.book.active_id;
    assert(editor.add_sheet("Observer"));
    const uint32_t observer = editor.book.active_id;
    assert(editor.set_cell(source, 0, 0, "=Z100"));
    assert(editor.set_cell(source, 0, 2, "=A1"));
    assert(editor.set_cell(source, 1, 0, "1"));
    assert(editor.set_cell(source, 2, 0, "2"));
    assert(editor.set_cell(target, 1, 1, "='Sheet1'!A1"));
    assert(editor.set_cell(observer, 0, 0, "='Sheet1'!A1&\" Sheet1!A1\""));

    rx::Clipboard clipboard;
    assert(editor.copy_range(source, {0, 0, 0, 0}, clipboard, true));
    assert(editor.paste(target, 3, 3, clipboard));
    assert(editor.book.get(target, 3, 3)->input == "='Sheet1'!Z100");
    assert(editor.book.get(source, 0, 2)->input == "='Target'!D4");
    assert(editor.book.get(target, 1, 1)->input == "='Target'!D4");
    assert(editor.book.get(observer, 0, 0)->input == "='Target'!D4&\" Sheet1!A1\"");
    assert(editor.undo());
    assert(editor.book.get(source, 0, 0)->input == "=Z100");
    assert(editor.book.get(observer, 0, 0)->input == "='Sheet1'!A1&\" Sheet1!A1\"");

    assert(editor.set_cell(source, 2, 2, "=SUM(A1:A2)"));
    assert(editor.copy_range(source, {0, 0, 1, 0}, clipboard, true));
    assert(editor.paste(target, 5, 0, clipboard));
    assert(editor.book.get(source, 2, 2)->input == "=SUM('Target'!A6:'Target'!A7)");
}

static void test_cut_rejects_partial_dependent_range_overlap_atomically() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "1"));
    assert(editor.set_cell(id, 1, 0, "2"));
    assert(editor.set_cell(id, 0, 2, "=SUM(A1:A3)"));
    rx::Clipboard clipboard;
    assert(editor.copy_range(id, {0, 0, 1, 0}, clipboard, true));
    const uint64_t revision = editor.book.revision;
    assert(!editor.paste(id, 0, 1, clipboard));
    assert(editor.book.revision == revision);
    assert(editor.book.get(id, 0, 0)->input == "1");
    assert(editor.book.get(id, 1, 0)->input == "2");
    assert(editor.book.get(id, 0, 2)->input == "=SUM(A1:A3)");
    assert(editor.book.get(id, 0, 1) == nullptr);
    assert(clipboard.cut);
    assert(editor.error == "剪切范围与公式引用范围部分重叠，操作未执行");
}

static void test_cancelled_cut_does_not_change_book_or_clipboard() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t source = editor.book.active_id;
    assert(editor.add_sheet("Target"));
    const uint32_t target = editor.book.active_id;
    assert(editor.set_cell(source, 0, 0, "5"));
    assert(editor.set_cell(source, 0, 1, "=A1"));
    rx::Clipboard clipboard;
    assert(editor.copy_range(source, {0, 0, 0, 0}, clipboard, true));
    const uint64_t revision = editor.book.revision;
    editor.progress = cancel_progress;
    assert(!editor.paste(target, 2, 2, clipboard));
    assert(editor.book.revision == revision);
    assert(editor.book.get(source, 0, 0)->input == "5");
    assert(editor.book.get(source, 0, 1)->input == "=A1");
    assert(editor.book.get(target, 2, 2) == nullptr);
    assert(clipboard.cut);
}

static void test_structural_edits_rewrite_references_and_undo() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 1, 0, "kept"));
    assert(editor.set_cell(id, 0, 2, "=$A$2+A2"));
    assert(editor.insert_axis(id, true, 1, 1));
    assert(editor.book.get(id, 2, 0)->input == "kept");
    assert(editor.book.get(id, 0, 2)->input == "=$A$3+A3");
    assert(editor.delete_axis(id, true, 2, 1));
    assert(strstr(editor.book.get(id, 0, 2)->input.c_str(), "#REF!") != nullptr);
    assert(editor.undo());
    assert(editor.book.get(id, 2, 0)->input == "kept");
    assert(editor.book.get(id, 0, 2)->input == "=$A$3+A3");
}

static void test_merge_guards_and_discard() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "keep"));
    assert(editor.set_cell(id, 0, 1, "must-confirm"));
    assert(!editor.merge(id, {0, 0, 0, 1}, false, false));
    assert(editor.book.get(id, 0, 1)->input == "must-confirm");
    assert(editor.merge(id, {0, 0, 0, 1}, false, true));
    assert(editor.book.get(id, 0, 0)->input == "keep");
    assert(editor.book.get(id, 0, 1) == nullptr);
    assert(!editor.insert_axis(id, false, 1, 1));
    assert(editor.merge(id, {0, 0, 0, 1}, true));
    assert(editor.book.sheet(id)->merges.empty());
}

static bool cancel_progress(void *, unsigned, unsigned) { return false; }

static void test_sort_filter_replace_and_sheet_history() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "商品"));
    assert(editor.set_cell(id, 0, 1, "数量"));
    assert(editor.set_cell(id, 1, 0, "梨"));
    assert(editor.set_cell(id, 1, 1, "3"));
    assert(editor.set_cell(id, 2, 0, "苹果"));
    assert(editor.set_cell(id, 2, 1, "1"));
    assert(editor.sort(id, {0, 0, 2, 1}, 1, false, true));
    assert(editor.book.get(id, 1, 0)->input == "苹果");
    assert(editor.book.get(id, 1, 1)->input == "1");

    assert(editor.set_filter(id, {0, 0, 2, 1}, 0, "梨", 1, true));
    assert(!editor.book.sheet(id)->row_visible(1));
    assert(editor.book.sheet(id)->row_visible(2));
    assert(editor.clear_filter(id));
    assert(editor.book.sheet(id)->row_visible(1));

    unsigned replaced = 0;
    assert(editor.set_cell(id, 3, 0, "梨梨"));
    assert(editor.replace_all(id, "梨", "桃", true, &replaced));
    assert(replaced == 3);
    assert(editor.book.get(id, 3, 0)->input == "桃桃");
    assert(editor.book.get(id, 2, 0)->input == "桃");
    assert(editor.undo());
    assert(editor.book.get(id, 3, 0)->input == "梨梨");
    assert(editor.book.get(id, 2, 0)->input == "梨");

    assert(editor.add_sheet("第二季度"));
    const uint32_t added = editor.book.active_id;
    assert(editor.rename_sheet(added, "季度二"));
    assert(editor.book.sheet(added)->name == "季度二");
    assert(editor.move_sheet(added, -1));
    assert(editor.book.sheets[0].id == added);
    assert(editor.delete_sheet(added));
    assert(editor.book.sheets.size() == 1);
}

static void test_sort_does_not_move_cells_outside_selected_columns() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "商品"));
    assert(editor.set_cell(id, 0, 1, "数量"));
    assert(editor.set_cell(id, 1, 0, "梨"));
    assert(editor.set_cell(id, 1, 1, "3"));
    assert(editor.set_cell(id, 1, 2, "=$D$1"));
    assert(editor.set_cell(id, 2, 0, "苹果"));
    assert(editor.set_cell(id, 2, 1, "1"));
    assert(editor.set_cell(id, 2, 2, "=$D$2"));

    assert(editor.sort(id, {0, 0, 2, 1}, 1, false, true));
    assert(editor.book.get(id, 1, 0)->input == "苹果");
    assert(editor.book.get(id, 2, 0)->input == "梨");
    assert(editor.book.get(id, 1, 2)->input == "=$D$1");
    assert(editor.book.get(id, 2, 2)->input == "=$D$2");
}

static void test_bulk_paste_merges_sparse_cells_and_undoes() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    constexpr int count = 1200;
    for (int i = 0; i < count; ++i) {
        char keep[32], source[32];
        snprintf(keep, sizeof(keep), "keep-%d", i);
        snprintf(source, sizeof(source), "paste-%d", i);
        assert(editor.set_cell(id, 4000 + i, 0, keep));
        assert(editor.set_cell(id, 6000 + i, 0, source));
    }

    rx::Clipboard clipboard;
    assert(editor.copy_range(id, {6000, 0, 6000 + count - 1, 0}, clipboard));
    assert(editor.paste(id, 0, 0, clipboard));
    assert(editor.book.get(id, 0, 0)->input == "paste-0");
    assert(editor.book.get(id, count - 1, 0)->input == "paste-1199");
    assert(editor.book.get(id, 4000, 0)->input == "keep-0");
    assert(editor.book.get(id, 4000 + count - 1, 0)->input == "keep-1199");
    assert(editor.undo());
    assert(editor.book.get(id, 0, 0) == nullptr);
    assert(editor.book.get(id, 4000, 0)->input == "keep-0");
}

static void test_cancelled_sort_does_not_change_cells() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.set_cell(id, 0, 0, "z"));
    assert(editor.set_cell(id, 1, 0, "a"));
    const uint64_t revision = editor.book.revision;
    editor.progress = cancel_progress;
    assert(!editor.sort(id, {0, 0, 1, 0}, 0, false, false));
    assert(editor.book.revision == revision);
    assert(editor.book.get(id, 0, 0)->input == "z");
    assert(editor.book.get(id, 1, 0)->input == "a");
}

static void test_view_and_resize_changes_are_undoable() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    assert(editor.resize_axis(id, true, 2, 2, 40));
    assert(editor.book.sheet(id)->row_heights.size() == 4);
    assert(editor.book.sheet(id)->row_heights[2] == 40);
    assert(editor.undo());
    assert(editor.book.sheet(id)->row_heights.empty());
    assert(editor.redo());
    assert(editor.book.sheet(id)->row_heights[3] == 40);

    assert(editor.resize_axis(id, false, 1, 1, 140));
    assert(editor.book.sheet(id)->column_widths[1] == 140);
    assert(editor.set_view(id, true, true, false, 125));
    assert(editor.book.sheet(id)->freeze_row);
    assert(editor.book.sheet(id)->freeze_column);
    assert(!editor.book.sheet(id)->gridlines);
    assert(editor.book.sheet(id)->zoom == 125);
    assert(editor.undo());
    assert(!editor.book.sheet(id)->freeze_row);
    assert(editor.book.sheet(id)->gridlines);
    assert(editor.book.sheet(id)->zoom == 100);
}

static void test_patch_style_changes_only_masked_properties_and_undoes_once() {
    rx::Editor editor;
    assert(editor.book.init());
    const uint32_t id = editor.book.active_id;
    rx::Style first;
    first.foreground = 0x123456;
    first.background = 0xffe0a0;
    first.size = 18;
    first.italic = 1;
    first.format = rx::NumberFormat::Percent;
    rx::Style second;
    second.foreground = 0x654321;
    second.alignment = rx::Alignment::Right;
    second.wrap = 1;
    assert(editor.book.sheet(id)->set_style(0, 0, first));
    assert(editor.book.sheet(id)->set_style(0, 1, second));
    const rx::Style original_first = first;
    const rx::Style original_second = second;

    rx::Style patch;
    patch.bold = 1;
    patch.foreground = 0xff0000;
    patch.size = 16;
    patch.format = rx::NumberFormat::Number;
    assert(editor.patch_style(id, {0, 0, 0, 1}, patch, 1 | 4 | 8 | 256));
    first.bold = 1;
    first.foreground = 0xff0000;
    first.size = 16;
    first.format = rx::NumberFormat::Number;
    second.bold = 1;
    second.foreground = 0xff0000;
    second.size = 16;
    second.format = rx::NumberFormat::Number;
    assert(editor.book.get(id, 0, 0)->style == first);
    assert(editor.book.get(id, 0, 1)->style == second);
    assert(editor.undo());
    assert(editor.book.get(id, 0, 0)->style == original_first);
    assert(editor.book.get(id, 0, 1)->style == original_second);
}

int main() {
    test_utf8_and_addresses();
    test_cut_paste_preserves_moved_formula_references();
    test_sparse_model_clone_and_formatting();
    test_set_cell_history_and_rejection_are_atomic();
    test_copy_paste_fill_and_clear_restore_style();
    test_structural_edits_rewrite_references_and_undo();
    test_merge_guards_and_discard();
    test_sort_filter_replace_and_sheet_history();
    test_sort_does_not_move_cells_outside_selected_columns();
    test_bulk_paste_merges_sparse_cells_and_undoes();
    test_cut_paste_between_sheets_is_undoable();
    test_cut_retargets_same_sheet_dependents_and_moved_formulas();
    test_overlapping_cut_retargets_formulas_and_undo_redo();
    test_cut_retargets_cross_sheet_references_and_preserves_moved_context();
    test_cut_rejects_partial_dependent_range_overlap_atomically();
    test_cancelled_cut_does_not_change_book_or_clipboard();
    test_cancelled_sort_does_not_change_cells();
    test_view_and_resize_changes_are_undoable();
    test_patch_style_changes_only_masked_properties_and_undoes_once();
    return 0;
}
