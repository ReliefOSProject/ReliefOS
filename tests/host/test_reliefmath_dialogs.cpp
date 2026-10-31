#include "library.h"

#include <assert.h>
#include <string.h>

namespace {

void test_favorites_close_waits_for_io_and_guards_dirty_changes() {
    using Decision = rm::FavoriteCloseDecision;

    assert(rm::favorite_close_decision(false, false, false, false) ==
           Decision::None);
    assert(rm::favorite_close_decision(true, true, false, false) ==
           Decision::WaitForIo);
    assert(rm::favorite_close_decision(true, true, true, true) ==
           Decision::WaitForIo);
    assert(rm::favorite_close_decision(true, false, true, false) ==
           Decision::Save);
    assert(rm::favorite_close_decision(true, false, true, true) ==
           Decision::ConfirmDiscard);
    assert(rm::favorite_close_decision(true, false, false, false) ==
           Decision::Close);
}

void test_cases_column_limit_matches_parser_support() {
    assert(rm::structure_column_limit(rm::Tool::Cases) == 2);
    assert(rm::structure_column_limit(rm::Tool::Matrix) == 16);
    assert(rm::structure_column_limit(rm::Tool::Aligned) == 16);
}

void test_completion_replaces_the_whole_command_when_caret_is_inside_it() {
    const char source[] = "x+\\sqrt";
    rm::Text prefix;
    rm::Range replacement{};
    rm::Error error;

    assert(rm::command_completion_prefix(source, 5, prefix, replacement, error));
    assert(prefix == "\\sq");
    assert(replacement.begin == 2);
    assert(replacement.end == 7);
}

void test_completion_rejects_a_caret_outside_a_backslash_command() {
    const char source[] = "x + sqrt";
    rm::Text prefix;
    rm::Range replacement{};
    rm::Error error;

    assert(!rm::command_completion_prefix(source, 6, prefix, replacement, error));
    assert(error.code == rm::Code::NotFound);
}

void test_completion_rejects_escaped_commands_and_split_utf8_carets() {
    rm::Text prefix;
    rm::Range replacement{};
    rm::Error error;

    assert(!rm::command_completion_prefix("\\\\alpha", 7, prefix,
                                          replacement, error));
    assert(error.code == rm::Code::NotFound);
    assert(!rm::command_completion_prefix("中文\\alpha", 1, prefix,
                                          replacement, error));
    assert(error.code == rm::Code::Format);
}

void test_replace_all_preserves_utf8_and_replaces_adjacent_matches() {
    rm::Text output;
    rm::Error error;
    size_t count = 0;

    assert(rm::replace_all_utf8("a中文中文b", "中文", "式", output, count, error));
    assert(output == "a式式b");
    assert(count == 2);
}

void test_replace_all_empty_search_is_rejected_without_changing_output() {
    rm::Text output;
    assert(output.set("保留"));
    rm::Error error;
    size_t count = 99;

    assert(!rm::replace_all_utf8("a", "", "b", output, count, error));
    assert(output == "保留");
    assert(count == 0);
    assert(error.code == rm::Code::Format);
}

void test_catalog_search_accepts_chinese_aliases_and_command_text() {
    const rm::ToolEntry *root = rm::catalog_find("structure.root");
    const rm::ToolEntry *alpha = rm::catalog_find("greek.alpha");
    assert(root && alpha);

    assert(rm::catalog_entry_matches(*root, "Square root", "Roots", "根号"));
    assert(rm::catalog_entry_matches(*alpha, "Alpha", "Greek letters", "\\alpha"));
    assert(!rm::catalog_entry_matches(*alpha, "Alpha", "Greek letters", "积分"));
}

void test_catalog_chinese_aliases_distinguish_operators_relations_and_functions() {
    const rm::ToolEntry *plus = rm::catalog_find("operator.plus");
    const rm::ToolEntry *minus = rm::catalog_find("operator.minus");
    const rm::ToolEntry *less = rm::catalog_find("relation.less");
    const rm::ToolEntry *greater = rm::catalog_find("relation.greater");
    const rm::ToolEntry *sine = rm::catalog_find("function.sin");
    const rm::ToolEntry *cosine = rm::catalog_find("function.cos");
    const rm::ToolEntry *logarithm = rm::catalog_find("function.log");
    assert(plus && minus && less && greater && sine && cosine && logarithm);

    assert(rm::catalog_entry_matches(*plus, "Plus", "Binary operators", "加号"));
    assert(rm::catalog_entry_matches(*minus, "Minus", "Binary operators", "运算符"));
    assert(!rm::catalog_entry_matches(*minus, "Minus", "Binary operators", "加号"));
    assert(rm::catalog_entry_matches(*less, "Less than", "Relations", "小于"));
    assert(!rm::catalog_entry_matches(*greater, "Greater than", "Relations", "小于"));
    assert(rm::catalog_entry_matches(*sine, "Sine", "Functions", "正弦"));
    assert(!rm::catalog_entry_matches(*cosine, "Cosine", "Functions", "正弦"));
    assert(rm::catalog_entry_matches(*logarithm, "Logarithm", "Functions", "对数"));
    assert(!rm::catalog_entry_matches(*sine, "Sine", "Functions", "对数"));
}

} // namespace

int main() {
    test_favorites_close_waits_for_io_and_guards_dirty_changes();
    test_cases_column_limit_matches_parser_support();
    test_completion_replaces_the_whole_command_when_caret_is_inside_it();
    test_completion_rejects_a_caret_outside_a_backslash_command();
    test_completion_rejects_escaped_commands_and_split_utf8_carets();
    test_replace_all_preserves_utf8_and_replaces_adjacent_matches();
    test_replace_all_empty_search_is_rejected_without_changing_output();
    test_catalog_search_accepts_chinese_aliases_and_command_text();
    test_catalog_chinese_aliases_distinguish_operators_relations_and_functions();
    return 0;
}
