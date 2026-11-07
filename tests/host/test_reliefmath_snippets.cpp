#include "catalog.h"
#include "editor_core.h"
#include "snippet.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern "C" void *__real_malloc(size_t size);
extern "C" void *__real_realloc(void *memory, size_t size);

namespace {

bool watch_allocations = false;
size_t allocation_ceiling = 0;
size_t oversized_allocations = 0;

bool reject_oversized_allocation(size_t size) {
    if (!watch_allocations || size <= allocation_ceiling) return false;
    ++oversized_allocations;
    return true;
}

void watch_source_allocations(size_t ceiling = 1024) {
    // A Limit error can still allocate its short diagnostic message.
    allocation_ceiling = ceiling;
    oversized_allocations = 0;
    watch_allocations = true;
}

void expect_limit_without_large_allocation(bool result, const rm::Error &error) {
    watch_allocations = false;
    if (result || error.code != rm::Code::Limit || oversized_allocations) {
        fprintf(stderr, "limit check: result=%d, code=%u, oversized allocations=%zu\n",
                result, static_cast<unsigned>(error.code), oversized_allocations);
    }
    assert(!result);
    assert(error.code == rm::Code::Limit);
    assert(oversized_allocations == 0);
}

void expect_text(const rm::Text &actual, const char *expected) {
    assert(actual.c_str() != 0);
    if (actual.size() != strlen(expected) || strcmp(actual.c_str(), expected) != 0) {
        fprintf(stderr, "text mismatch: got %zu bytes [%s], expected %zu bytes [%s]\n",
                actual.size(), actual.c_str(), strlen(expected), expected);
    }
    assert(actual.size() == strlen(expected));
    assert(strcmp(actual.c_str(), expected) == 0);
}

bool has_kind(const rm::LexToken *tokens, size_t count, rm::TokenKind kind) {
    for (size_t i = 0; i < count; ++i) {
        if (tokens[i].kind == kind) {
            return true;
        }
    }
    return false;
}

bool all_kinds_are_not_error(const rm::LexToken *tokens, size_t count) {
    return !has_kind(tokens, count, rm::TokenKind::Error);
}

void test_catalog_is_searchable_and_every_entry_builds() {
    size_t count = 0;
    const rm::ToolEntry *entries = rm::catalog_entries(&count);
    assert(entries != 0);
    assert(count >= 80);

    rm::ToolArgs args{};
    rm::Error error;
    for (size_t i = 0; i < count; ++i) {
        assert(entries[i].key != 0 && entries[i].key[0] != '\0');
        assert(entries[i].msgid != 0 && entries[i].msgid[0] != '\0');
        assert(entries[i].category != 0 && entries[i].category[0] != '\0');
        assert(entries[i].command != 0 && entries[i].command[0] != '\0');
        assert(entries[i].insert != 0 && entries[i].insert[0] != '\0');
        assert(entries[i].parameters != 0);
        assert(entries[i].help_example != 0 && entries[i].help_example[0] != '\0');
        assert(rm::catalog_find(entries[i].key) == &entries[i]);

        rm::Snippet snippet;
        assert(rm::build_snippet(entries[i].tool, args, snippet, error));
        assert(snippet.source.size() != 0);
        if (strcmp(entries[i].parameters, "none") != 0) {
            assert(!snippet.stops.empty());
        }
        for (size_t stop = 0; stop < snippet.stops.size(); ++stop) {
            assert(snippet.stops[stop].begin < snippet.stops[stop].end);
            assert(snippet.stops[stop].end <= snippet.source.size());
            assert(rm::detail::valid_utf8(
                snippet.source.c_str() + snippet.stops[stop].begin,
                snippet.stops[stop].end - snippet.stops[stop].begin));
        }
    }

    const rm::ToolEntry *matches[16];
    size_t match_count = rm::catalog_search("square root", matches, 16);
    assert(match_count > 0);
    bool saw_root = false;
    for (size_t i = 0; i < match_count; ++i) {
        if (strcmp(matches[i]->key, "structure.root") == 0) {
            saw_root = true;
        }
    }
    assert(saw_root);
}

void test_fraction_uses_the_specified_byte_ranges() {
    rm::Snippet snippet;
    rm::Error error;
    rm::ToolArgs args{};
    assert(rm::build_snippet(rm::Tool::Fraction, args, snippet, error));
    expect_text(snippet.source, "\\frac{a}{b}");
    assert(snippet.stops.size() == 2);
    assert(snippet.stops[0].begin == 6 && snippet.stops[0].end == 7);
    assert(snippet.stops[1].begin == 9 && snippet.stops[1].end == 10);
}

void test_numbered_root_has_editable_index_and_radicand() {
    rm::ToolArgs args{};
    args.numbered_root = true;
    rm::Snippet snippet;
    rm::Error error;
    assert(rm::build_snippet(rm::Tool::Root, args, snippet, error));
    expect_text(snippet.source, "\\sqrt[n]{x}");
    assert(snippet.stops.size() == 2);
    assert(snippet.stops[0].begin == 6 && snippet.stops[0].end == 7);
    assert(snippet.stops[1].begin == 9 && snippet.stops[1].end == 10);
}

void test_matrix_dimension_limit_fails_without_partial_output() {
    rm::ToolArgs args{};
    args.rows = 17;
    args.columns = 2;
    rm::Snippet snippet;
    rm::Error error;
    assert(!rm::build_snippet(rm::Tool::Matrix, args, snippet, error));
    assert(error.code == rm::Code::Limit);
    assert(snippet.source.empty());
    assert(snippet.stops.empty());
}

void test_selected_utf8_text_is_wrapped_without_splitting_bytes() {
    rm::ToolArgs args{};
    assert(args.selected.set("中文", 6));
    rm::Snippet snippet;
    rm::Error error;
    assert(rm::build_snippet(rm::Tool::Text, args, snippet, error));
    expect_text(snippet.source, "\\text{中文}");
    assert(snippet.stops.size() == 1);
    assert(snippet.stops[0].begin == 6 && snippet.stops[0].end == 12);
}

void test_matrix_creates_one_nonempty_stop_per_cell() {
    rm::ToolArgs args{};
    args.rows = 2;
    args.columns = 3;
    rm::Snippet snippet;
    rm::Error error;
    assert(rm::build_snippet(rm::Tool::Matrix, args, snippet, error));
    expect_text(snippet.source,
                "\\begin{matrix}a & b & c \\\\ d & e & f\\end{matrix}");
    assert(snippet.stops.size() == 6);
    const char expected[] = {'a', 'b', 'c', 'd', 'e', 'f'};
    for (size_t i = 0; i < 6; ++i) {
        const rm::Range stop = snippet.stops[i];
        assert(stop.end == stop.begin + 1);
        assert(snippet.source.c_str()[stop.begin] == expected[i]);
    }
}

void test_nested_structure_inserts_into_active_placeholder() {
    rm::EditorCore editor;
    rm::Error error;
    rm::Snippet fraction;
    rm::Snippet superscript;
    rm::ToolArgs args{};
    assert(rm::build_snippet(rm::Tool::Fraction, args, fraction, error));
    assert(rm::build_snippet(rm::Tool::Superscript, args, superscript, error));
    assert(rm::editor_set_text(editor, "", error));
    assert(rm::editor_insert_snippet(editor, {0, 0}, fraction, error));

    bool handled = false;
    rm::Range selected{};
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(handled);
    assert(selected.begin == 6 && selected.end == 7);
    assert(rm::editor_insert_snippet(editor, selected, superscript, error));
    expect_text(editor.source, "\\frac{a^{b}}{b}");
    assert(editor.stops.size() == 3);
    assert(editor.stops[0].begin == 6 && editor.stops[0].end == 7);
    assert(editor.stops[1].begin == 9 && editor.stops[1].end == 10);
    assert(editor.stops[2].begin == 13 && editor.stops[2].end == 14);
}

void test_editing_one_placeholder_moves_later_ranges() {
    rm::EditorCore editor;
    rm::Error error;
    rm::Snippet fraction;
    rm::ToolArgs args{};
    assert(rm::build_snippet(rm::Tool::Fraction, args, fraction, error));
    assert(rm::editor_set_text(editor, "", error));
    assert(rm::editor_insert_snippet(editor, {0, 0}, fraction, error));
    assert(rm::editor_replace_text(editor, {6, 7}, "numerator", error));
    expect_text(editor.source, "\\frac{numerator}{b}");
    assert(editor.stops.size() == 2);
    assert(editor.stops[0].begin == 6 && editor.stops[0].end == 15);
    assert(editor.stops[1].begin == 17 && editor.stops[1].end == 18);
}

void test_cross_placeholder_delete_invalidates_stops_and_undo_restores_them() {
    rm::EditorCore editor;
    rm::Error error;
    rm::Snippet fraction;
    rm::ToolArgs args{};
    assert(rm::build_snippet(rm::Tool::Fraction, args, fraction, error));
    assert(rm::editor_set_text(editor, "", error));
    assert(rm::editor_insert_snippet(editor, {0, 0}, fraction, error));
    assert(rm::editor_replace_text(editor, {6, 10}, "", error));
    expect_text(editor.source, "\\frac{}");
    assert(editor.stops.size() == 0);

    bool handled = true;
    rm::Range selected{};
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(!handled);

    assert(rm::editor_undo(editor, error));
    expect_text(editor.source, "\\frac{a}{b}");
    assert(editor.stops.size() == 2);
    assert(editor.stops[0].begin == 6 && editor.stops[0].end == 7);
    assert(editor.stops[1].begin == 9 && editor.stops[1].end == 10);
    assert(rm::editor_undo(editor, error));
    expect_text(editor.source, "");
    assert(editor.stops.empty());
}

void test_tab_and_shift_tab_navigate_placeholders_then_yield() {
    rm::EditorCore editor;
    rm::Error error;
    rm::Snippet fraction;
    rm::ToolArgs args{};
    assert(rm::build_snippet(rm::Tool::Fraction, args, fraction, error));
    assert(rm::editor_set_text(editor, "", error));
    assert(rm::editor_insert_snippet(editor, {0, 0}, fraction, error));

    bool handled = false;
    rm::Range selected{};
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(handled && selected.begin == 6 && selected.end == 7);
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(handled && selected.begin == 9 && selected.end == 10);
    assert(rm::editor_tab(editor, true, handled, selected, error));
    assert(handled && selected.begin == 6 && selected.end == 7);
    assert(rm::editor_tab(editor, true, handled, selected, error));
    assert(!handled);
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(handled && selected.begin == 6 && selected.end == 7);
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(handled && selected.begin == 9 && selected.end == 10);
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(!handled);
}

void test_command_completion_finds_sq_prefix() {
    const rm::ToolEntry *matches[8];
    size_t count = rm::complete_command("\\sq", matches, 8);
    assert(count > 0 && count <= 8);
    bool saw_sqrt = false;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(matches[i]->command, "\\sqrt") == 0) {
            saw_sqrt = true;
        }
    }
    assert(saw_sqrt);
}

void test_utf8_find_replace_obeys_codepoint_boundaries() {
    rm::Error error;
    rm::Range found{};
    const char source[] = "αx中文y";
    assert(rm::find_text(source, "中文", 0, found, error));
    assert(found.begin == 3 && found.end == 9);

    rm::Text text;
    assert(text.set(source));
    assert(rm::replace_text(text, found, "公式", error));
    expect_text(text, "αx公式y");

    rm::Error invalid_error;
    const char before[] = "αx公式y";
    assert(text.set(before));
    assert(!rm::replace_text(text, {1, 2}, "z", invalid_error));
    expect_text(text, before);
}

void test_atomic_symbols_allow_zero_stops_and_yield_tab() {
    const rm::Tool tools[] = {rm::Tool::Alpha, rm::Tool::Plus, rm::Tool::ThinSpace};
    rm::ToolArgs args{};
    rm::Error error;
    for (rm::Tool tool : tools) {
        rm::Snippet snippet;
        assert(rm::build_snippet(tool, args, snippet, error));
        assert(!snippet.source.empty());
        assert(snippet.stops.empty());
        rm::EditorCore editor;
        assert(rm::editor_insert_snippet(editor, {0, 0}, snippet, error));
        bool handled = true;
        rm::Range selection{};
        assert(rm::editor_tab(editor, false, handled, selection, error));
        assert(!handled);
        handled = true;
        assert(rm::editor_tab(editor, true, handled, selection, error));
        assert(!handled);
    }
}

void test_lexer_enforces_source_limit_before_scanning() {
    char source[rm::kMaxSourceBytes + 2];
    memset(source, 'x', sizeof(source));
    source[rm::kMaxSourceBytes] = '\0';
    rm::LexToken tokens[1];
    size_t count = 0;
    rm::Error error;
    assert(rm::lex_source(source, rm::kMaxSourceBytes, tokens, 1, count, error));
    assert(count == 1 && tokens[0].range.end == rm::kMaxSourceBytes);

    source[rm::kMaxSourceBytes] = 'x';
    source[rm::kMaxSourceBytes + 1] = '\0';
    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::lex_source(source, rm::kMaxSourceBytes + 1, tokens, 1, count, error), error);
    assert(count == 0);

    // The explicit length must be rejected before reading any source bytes.
    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::lex_source("x", SIZE_MAX, tokens, 1, count, error), error);
    assert(count == 0);
}

void test_find_enforces_source_and_needle_limits() {
    char source[rm::kMaxSourceBytes + 2];
    memset(source, 'x', sizeof(source));
    source[rm::kMaxSourceBytes - 1] = 'z';
    source[rm::kMaxSourceBytes] = '\0';
    rm::Range found{};
    rm::Error error;
    assert(rm::find_text(source, "z", 0, found, error));
    assert(found.begin == rm::kMaxSourceBytes - 1 && found.end == rm::kMaxSourceBytes);

    source[rm::kMaxSourceBytes] = 'x';
    source[rm::kMaxSourceBytes + 1] = '\0';
    const rm::Range before = found;
    watch_source_allocations();
    expect_limit_without_large_allocation(rm::find_text(source, "z", 0, found, error), error);
    assert(found.begin == before.begin && found.end == before.end);
    watch_source_allocations();
    expect_limit_without_large_allocation(rm::find_text("x", source, 0, found, error), error);
    assert(found.begin == before.begin && found.end == before.end);
}

void test_bracket_matcher_enforces_source_limit() {
    char source[rm::kMaxSourceBytes + 2];
    memset(source, 'x', sizeof(source));
    source[0] = '{';
    source[rm::kMaxSourceBytes - 1] = '}';
    source[rm::kMaxSourceBytes] = '\0';
    rm::Range match{};
    rm::Error error;
    assert(rm::matching_bracket(source, 0, match, error));
    assert(match.begin == rm::kMaxSourceBytes - 1 && match.end == rm::kMaxSourceBytes);

    source[rm::kMaxSourceBytes - 1] = 'x';
    source[rm::kMaxSourceBytes] = '}';
    source[rm::kMaxSourceBytes + 1] = '\0';
    const rm::Range before = match;
    watch_source_allocations();
    expect_limit_without_large_allocation(rm::matching_bracket(source, 0, match, error), error);
    assert(match.begin == before.begin && match.end == before.end);
}

void test_replacement_limit_precedes_allocation_and_preserves_text() {
    char source[rm::kMaxSourceBytes + 2];
    memset(source, 'x', sizeof(source));
    source[rm::kMaxSourceBytes] = '\0';
    rm::Text text;
    rm::Error error;
    assert(text.set(source));
    assert(rm::replace_text(text, {rm::kMaxSourceBytes - 1, rm::kMaxSourceBytes}, "y", error));
    assert(text.size() == rm::kMaxSourceBytes);
    assert(text.c_str()[rm::kMaxSourceBytes - 1] == 'y');
    const rm::Text before = text;

    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::replace_text(text, {rm::kMaxSourceBytes, rm::kMaxSourceBytes}, "x", error), error);
    assert(text == before);
    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::replace_text(text, {0, 1}, before.c_str(), error), error);
    assert(text == before);
    source[rm::kMaxSourceBytes] = 'x';
    source[rm::kMaxSourceBytes + 1] = '\0';
    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::replace_text(text, {0, rm::kMaxSourceBytes}, source, error), error);
    assert(text == before);
}

size_t initialize_full_editor(rm::EditorCore &editor, rm::Snippet &fraction,
                              rm::Error &error) {
    rm::ToolArgs args{};
    assert(rm::build_snippet(rm::Tool::Fraction, args, fraction, error));
    char prefix[rm::kMaxSourceBytes + 1];
    const size_t prefix_size = rm::kMaxSourceBytes - fraction.source.size();
    memset(prefix, 'x', prefix_size);
    prefix[prefix_size] = '\0';
    assert(rm::editor_set_text(editor, prefix, error));
    assert(rm::editor_insert_snippet(editor, {prefix_size, prefix_size}, fraction, error));
    assert(editor.source.size() == rm::kMaxSourceBytes);
    bool handled = false;
    rm::Range selected{};
    assert(rm::editor_tab(editor, false, handled, selected, error));
    assert(handled && selected.begin == prefix_size + 6 && selected.end == prefix_size + 7);
    return prefix_size;
}

void test_editor_replace_limit_preserves_stops_and_undo() {
    rm::EditorCore editor;
    rm::Snippet fraction;
    rm::Error error;
    const size_t base = initialize_full_editor(editor, fraction, error);
    const rm::Text before = editor.source;
    const rm::EditorUndoState *undo_before = editor.undo_top;
    const size_t depth_before = editor.undo_depth;
    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::editor_replace_text(editor, {base + 6, base + 7}, "zz", error), error);
    assert(editor.source == before);
    assert(editor.undo_top == undo_before && editor.undo_depth == depth_before);
    assert(editor.active_stop == 0 && editor.stops.size() == 2);
    assert(editor.stops[0].begin == base + 6 && editor.stops[0].end == base + 7);
    assert(editor.stops[1].begin == base + 9 && editor.stops[1].end == base + 10);
    assert(rm::editor_undo(editor, error));
    assert(editor.source.size() == base && editor.stops.empty());
}

void test_editor_insert_limit_preserves_stops_and_undo() {
    rm::EditorCore editor;
    rm::Snippet fraction;
    rm::Error error;
    const size_t base = initialize_full_editor(editor, fraction, error);
    const rm::Text before = editor.source;
    const rm::EditorUndoState *undo_before = editor.undo_top;
    const size_t depth_before = editor.undo_depth;
    watch_source_allocations();
    expect_limit_without_large_allocation(
        rm::editor_insert_snippet(editor, {base + 6, base + 7}, fraction, error), error);
    assert(editor.source == before);
    assert(editor.undo_top == undo_before && editor.undo_depth == depth_before);
    assert(editor.active_stop == 0 && editor.stops.size() == 2);
    assert(editor.stops[0].begin == base + 6 && editor.stops[0].end == base + 7);
    assert(editor.stops[1].begin == base + 9 && editor.stops[1].end == base + 10);
    assert(rm::editor_undo(editor, error));
    assert(editor.source.size() == base && editor.stops.empty());
}

void test_editor_set_limit_preserves_existing_state() {
    rm::EditorCore editor;
    rm::Snippet fraction;
    rm::Error error;
    const size_t base = initialize_full_editor(editor, fraction, error);
    const rm::Text before = editor.source;
    const rm::EditorUndoState *undo_before = editor.undo_top;
    const size_t depth_before = editor.undo_depth;
    char source[rm::kMaxSourceBytes + 2];
    memset(source, 'x', sizeof(source));
    source[rm::kMaxSourceBytes + 1] = '\0';
    watch_source_allocations();
    expect_limit_without_large_allocation(rm::editor_set_text(editor, source, error), error);
    assert(editor.source == before);
    assert(editor.undo_top == undo_before && editor.undo_depth == depth_before);
    assert(editor.active_stop == 0 && editor.stops.size() == 2);
    assert(rm::editor_undo(editor, error));
    assert(editor.source.size() == base && editor.stops.empty());

    source[rm::kMaxSourceBytes] = '\0';
    assert(rm::editor_set_text(editor, source, error));
    assert(editor.source.size() == rm::kMaxSourceBytes);
    assert(editor.undo_depth == 0 && editor.stops.empty());
}

void test_snippet_limit_bounds_buffer_growth() {
    char source[rm::kMaxSourceBytes + 1];
    memset(source, 'x', rm::kMaxSourceBytes);
    source[rm::kMaxSourceBytes] = '\0';
    rm::ToolArgs args{};
    rm::Snippet snippet;
    rm::Error error;

    // Six prefix bytes and one suffix byte leave exactly this much content.
    assert(args.selected.set(source, rm::kMaxSourceBytes - 7));
    watch_source_allocations(rm::kMaxSourceBytes + 64);
    const bool built = rm::build_snippet(rm::Tool::Text, args, snippet, error);
    watch_allocations = false;
    assert(built);
    assert(oversized_allocations == 0);
    assert(snippet.source.size() == rm::kMaxSourceBytes);
    assert(memcmp(snippet.source.c_str(), "\\text{", 6) == 0);
    assert(snippet.source.c_str()[rm::kMaxSourceBytes - 1] == '}');
    assert(snippet.stops.size() == 1);
    assert(snippet.stops[0].begin == 6 && snippet.stops[0].end == rm::kMaxSourceBytes - 1);
}

void test_snippet_limit_checks_each_append() {
    char source[rm::kMaxSourceBytes + 1];
    memset(source, 'x', rm::kMaxSourceBytes);
    source[rm::kMaxSourceBytes] = '\0';
    rm::ToolArgs args{};
    rm::Snippet snippet;
    rm::Error error;

    // Content fits; the final closing brace would exceed the limit.
    assert(args.selected.set(source, rm::kMaxSourceBytes - 6));
    watch_source_allocations(rm::kMaxSourceBytes + 1);
    expect_limit_without_large_allocation(
        rm::build_snippet(rm::Tool::Text, args, snippet, error), error);
    assert(snippet.source.empty() && snippet.stops.empty());

    // This content append already exceeds the limit before adding the suffix.
    assert(args.selected.set(source, rm::kMaxSourceBytes));
    watch_source_allocations(rm::kMaxSourceBytes + 1);
    expect_limit_without_large_allocation(
        rm::build_snippet(rm::Tool::Text, args, snippet, error), error);
    assert(snippet.source.empty() && snippet.stops.empty());
}

void test_allocation_failure_reports_error_and_preserves_text() {
    rm::Text text;
    assert(text.set("abc"));
    rm::Error error;
    rm::testing::fail_allocations_after(0);
    assert(!rm::replace_text(text, {1, 2}, "z", error));
    rm::testing::allow_allocations();
    assert(error.code == rm::Code::NoMemory);
    expect_text(text, "abc");

    rm::ToolArgs args{};
    rm::Snippet snippet;
    rm::Error snippet_error;
    rm::testing::fail_allocations_after(0);
    assert(!rm::build_snippet(rm::Tool::Fraction, args, snippet, snippet_error));
    rm::testing::allow_allocations();
    assert(snippet_error.code == rm::Code::NoMemory);
    assert(snippet.source.empty());
    assert(snippet.stops.empty());
}

void test_bracket_matching_handles_nesting_and_comments() {
    rm::Error error;
    rm::Range match{};
    const char source[] = "{a % } ignored\n + [b]}";
    assert(rm::matching_bracket(source, 0, match, error));
    assert(match.begin == strlen(source) - 1 && match.end == strlen(source));
    assert(rm::matching_bracket(source, match.begin, match, error));
    assert(match.begin == 0 && match.end == 1);

    const char escaped[] = "{\\{x\\}}";
    assert(rm::matching_bracket(escaped, 0, match, error));
    assert(match.begin == strlen(escaped) - 1 && match.end == strlen(escaped));
}

void test_lexer_marks_commands_groups_comments_and_errors() {
    const char source[] = "\\frac{a}{b} % note\n\\unknown";
    rm::LexToken tokens[64];
    size_t count = 0;
    rm::Error error;
    assert(rm::lex_source(source, strlen(source), tokens, 64, count, error));
    assert(has_kind(tokens, count, rm::TokenKind::Command));
    assert(has_kind(tokens, count, rm::TokenKind::GroupOpen));
    assert(has_kind(tokens, count, rm::TokenKind::GroupClose));
    assert(has_kind(tokens, count, rm::TokenKind::Comment));
    assert(has_kind(tokens, count, rm::TokenKind::Error));

    const char unicode_command[] = "\\α{x}";
    count = 0;
    rm::Error unicode_error;
    assert(rm::lex_source(unicode_command, strlen(unicode_command), tokens, 64,
                          count, unicode_error));
    assert(count >= 1 && tokens[0].kind == rm::TokenKind::Error);
    assert(tokens[0].range.begin == 0 && tokens[0].range.end == 3);
}

void test_formula_fixture_contains_only_supported_math() {
    FILE *file = fopen("tests/fixtures/reliefmath/formulas.tsv", "rb");
    assert(file != 0);
    size_t catalog_count = 0;
    const rm::ToolEntry *catalog = rm::catalog_entries(&catalog_count);
    assert(catalog_count <= 256);
    bool seen[256] = {};
    char line[2048];
    size_t rows = 0;
    while (fgets(line, sizeof(line), file) != 0) {
        char *tab = strchr(line, '\t');
        assert(tab != 0);
        *tab++ = '\0';
        size_t length = strlen(tab);
        while (length != 0 && (tab[length - 1] == '\n' || tab[length - 1] == '\r')) {
            tab[--length] = '\0';
        }
        assert(line[0] != '\0' && tab[0] != '\0');

        const rm::ToolEntry *entry = rm::catalog_find(line);
        assert(entry != 0);
        const size_t entry_index = static_cast<size_t>(entry - catalog);
        assert(entry_index < catalog_count && !seen[entry_index]);
        seen[entry_index] = true;

        rm::ToolArgs args{};
        rm::Snippet snippet;
        rm::Error build_error;
        assert(rm::build_snippet(entry->tool, args, snippet, build_error));
        expect_text(snippet.source, tab);

        rm::LexToken tokens[256];
        size_t count = 0;
        rm::Error error;
        assert(rm::lex_source(tab, length, tokens, 256, count, error));
        assert(all_kinds_are_not_error(tokens, count));
        ++rows;
    }
    assert(fclose(file) == 0);
    assert(rows == catalog_count);
    for (size_t index = 0; index < catalog_count; ++index) {
        assert(seen[index]);
    }
}

void test_invalid_fixture_is_rejected_by_the_lexer() {
    FILE *file = fopen("tests/fixtures/reliefmath/invalid.tsv", "rb");
    assert(file != 0);
    char line[2048];
    size_t rows = 0;
    while (fgets(line, sizeof(line), file) != 0) {
        char *tab = strchr(line, '\t');
        assert(tab != 0);
        *tab++ = '\0';
        size_t length = strlen(tab);
        while (length != 0 && (tab[length - 1] == '\n' || tab[length - 1] == '\r')) {
            tab[--length] = '\0';
        }
        assert(line[0] != '\0' && tab[0] != '\0');

        rm::LexToken tokens[256];
        size_t count = 0;
        rm::Error error;
        assert(rm::lex_source(tab, length, tokens, 256, count, error));
        assert(has_kind(tokens, count, rm::TokenKind::Error));
        ++rows;
    }
    assert(fclose(file) == 0);
    assert(rows >= 5);
}

} // namespace

extern "C" void *__wrap_malloc(size_t size) {
    if (reject_oversized_allocation(size)) return 0;
    return __real_malloc(size);
}

extern "C" void *__wrap_realloc(void *memory, size_t size) {
    if (reject_oversized_allocation(size)) return 0;
    return __real_realloc(memory, size);
}

int main(int argc, char **argv) {
    struct Test {
        const char *name;
        void (*run)();
    };
    const Test tests[] = {
        {"catalog", test_catalog_is_searchable_and_every_entry_builds},
        {"fraction", test_fraction_uses_the_specified_byte_ranges},
        {"root", test_numbered_root_has_editable_index_and_radicand},
        {"matrix-limit", test_matrix_dimension_limit_fails_without_partial_output},
        {"selected-utf8", test_selected_utf8_text_is_wrapped_without_splitting_bytes},
        {"matrix", test_matrix_creates_one_nonempty_stop_per_cell},
        {"nested", test_nested_structure_inserts_into_active_placeholder},
        {"stop-shift", test_editing_one_placeholder_moves_later_ranges},
        {"stop-undo", test_cross_placeholder_delete_invalidates_stops_and_undo_restores_them},
        {"tab", test_tab_and_shift_tab_navigate_placeholders_then_yield},
        {"completion", test_command_completion_finds_sq_prefix},
        {"utf8", test_utf8_find_replace_obeys_codepoint_boundaries},
        {"atomic-stops", test_atomic_symbols_allow_zero_stops_and_yield_tab},
        {"lexer-limit", test_lexer_enforces_source_limit_before_scanning},
        {"find-limit", test_find_enforces_source_and_needle_limits},
        {"bracket-limit", test_bracket_matcher_enforces_source_limit},
        {"replace-limit", test_replacement_limit_precedes_allocation_and_preserves_text},
        {"editor-replace-limit", test_editor_replace_limit_preserves_stops_and_undo},
        {"editor-insert-limit", test_editor_insert_limit_preserves_stops_and_undo},
        {"editor-set-limit", test_editor_set_limit_preserves_existing_state},
        {"snippet-growth-limit", test_snippet_limit_bounds_buffer_growth},
        {"snippet-limit", test_snippet_limit_checks_each_append},
        {"allocation-failure", test_allocation_failure_reports_error_and_preserves_text},
        {"brackets", test_bracket_matching_handles_nesting_and_comments},
        {"lexer", test_lexer_marks_commands_groups_comments_and_errors},
        {"valid-fixtures", test_formula_fixture_contains_only_supported_math},
        {"invalid-fixtures", test_invalid_fixture_is_rejected_by_the_lexer}
    };
    if (argc > 2) return 2;
    size_t passed = 0;
    for (const Test &test : tests) {
        if (argc == 2 && strcmp(argv[1], test.name) != 0) continue;
        test.run();
        ++passed;
    }
    if (!passed) {
        fprintf(stderr, "Unknown snippets test: %s\n", argv[1]);
        return 2;
    }
    printf("ReliefMath snippets: %zu tests passed\n", passed);
    return 0;
}
