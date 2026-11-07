#include "formula.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace rx;

static void expect_number(const Value &value, double expected)
{
    assert(value.kind == ValueKind::Number);
    assert(value.number == expected);
}

static void expect_boolean(const Value &value, bool expected)
{
    assert(value.kind == ValueKind::Boolean);
    assert(value.text == (expected ? "TRUE" : "FALSE"));
}

static void expect_text(const Value &value, const char *expected)
{
    assert(value.kind == ValueKind::Text);
    assert(value.text == expected);
}

static void expect_error(const Value &value, const char *expected)
{
    assert(value.kind == ValueKind::Error);
    assert(value.text == expected);
}

static void set(Workbook &book, uint32_t sheet, int row, int col,
                const char *input, bool literal = false)
{
    assert(book.set_input(sheet, row, col, input, literal));
}

static uint32_t add_sheet(Workbook &book, const char *name)
{
    Sheet sheet;
    sheet.id = book.next_id++;
    assert(sheet.name.set(name));
    uint32_t id = sheet.id;
    assert(book.sheets.push(static_cast<Sheet &&>(sheet)));
    return id;
}

static void test_operators_literals_and_errors(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    set(book, id, 0, 0, "=1+2*3");
    expect_number(evaluate(book, id, 0, 0), 7);
    set(book, id, 0, 1, "=(1+2)*3");
    expect_number(evaluate(book, id, 0, 1), 9);
    set(book, id, 0, 2, "=10/2-3");
    expect_number(evaluate(book, id, 0, 2), 2);
    set(book, id, 0, 3, "=\"你好\"&\"，世界\"");
    expect_text(evaluate(book, id, 0, 3), "你好，世界");
    set(book, id, 0, 4, "=TRUE");
    expect_boolean(evaluate(book, id, 0, 4), true);
    set(book, id, 0, 5, "=2<=2");
    expect_boolean(evaluate(book, id, 0, 5), true);
    set(book, id, 0, 6, "=\"a\"<>\"b\"");
    expect_boolean(evaluate(book, id, 0, 6), true);
    set(book, id, 0, 7, "=1/0");
    expect_error(evaluate(book, id, 0, 7), "#DIV/0!");
    set(book, id, 0, 8, "=UNKNOWNFUNC(1)");
    expect_error(evaluate(book, id, 0, 8), "#NAME?");
    set(book, id, 0, 11, "=LOG10(100)");
    expect_error(evaluate(book, id, 0, 11), "#NAME?");
    set(book, id, 0, 12, "=[External]Sheet1!A1");
    expect_error(evaluate(book, id, 0, 12), "#REF!");
    set(book, id, 1, 12, "=A1:A2");
    expect_error(evaluate(book, id, 1, 12), "#VALUE!");
    set(book, id, 1, 13, "=M2+1");
    expect_error(evaluate(book, id, 1, 13), "#VALUE!");
    set(book, id, 0, 9, "=1+");
    expect_error(evaluate(book, id, 0, 9), "#VALUE!");
    set(book, id, 0, 10, "=MissingSheet!A1");
    expect_error(evaluate(book, id, 0, 10), "#REF!");
}

static void test_references_functions_and_lazy_if(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    uint32_t sales = add_sheet(book, "销售 数据");
    set(book, sales, 0, 0, "1");
    set(book, sales, 1, 0, "2");
    set(book, sales, 2, 0, "apple");
    set(book, sales, 0, 1, "=SUM(A1:A3)");
    expect_number(evaluate(book, sales, 0, 1), 3);
    set(book, sales, 1, 1, "=AVERAGE(A1:A3)");
    expect_number(evaluate(book, sales, 1, 1), 1.5);
    set(book, sales, 2, 1, "=MIN(A1:A3)");
    expect_number(evaluate(book, sales, 2, 1), 1);
    set(book, sales, 3, 1, "=MAX(A1:A3)");
    expect_number(evaluate(book, sales, 3, 1), 2);
    set(book, sales, 4, 1, "=COUNT(A1:A4)");
    expect_number(evaluate(book, sales, 4, 1), 2);
    set(book, sales, 5, 1, "=COUNTA(A1:A4)");
    expect_number(evaluate(book, sales, 5, 1), 3);
    set(book, sales, 6, 1, "=COUNTIF(A1:A3,\">=2\")");
    expect_number(evaluate(book, sales, 6, 1), 1);
    set(book, sales, 7, 1, "=COUNTIF(A1:A3,\"a*\")");
    expect_number(evaluate(book, sales, 7, 1), 1);
    set(book, sales, 8, 1, "=SUMIF(A1:A3,\">1\",A1:A3)");
    expect_number(evaluate(book, sales, 8, 1), 2);
    set(book, sales, 9, 1, "=SUMIF(A1:B2,\">0\",A1:D1)");
    expect_error(evaluate(book, sales, 9, 1), "#VALUE!");
    set(book, sales, 4, 0, "=\"\"");
    set(book, sales, 5, 0, "*", true);
    set(book, sales, 10, 1, "=COUNTIF(A1:A6,\"~*\")");
    expect_number(evaluate(book, sales, 10, 1), 1);
    set(book, id, 0, 0, "='销售 数据'!$A$1+'销售 数据'!A2");
    expect_number(evaluate(book, id, 0, 0), 3);
    set(book, id, 0, 1, "=IF(FALSE,A2,42)");
    expect_number(evaluate(book, id, 0, 1), 42);
    set(book, id, 0, 2, "=IF(FALSE,A3,7)");
    expect_number(evaluate(book, id, 0, 2), 7);
    set(book, id, 0, 3, "=ROUND(1.234,2)+ABS(-3)");
    expect_number(evaluate(book, id, 0, 3), 4.23);
}

static void test_sumif_uses_evaluated_sum_range_values(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    set(book, id, 0, 0, "=1");
    set(book, id, 1, 0, "=2");
    set(book, id, 0, 1, "=A1*10");
    set(book, id, 1, 1, "=A2*10");
    set(book, id, 0, 2, "=SUMIF(A1:A2,\">1\",B1:B2)");

    expect_number(evaluate(book, id, 0, 2), 20);
}

static void test_sumif_propagates_errors_from_sum_range(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    set(book, id, 0, 0, "=1");
    set(book, id, 1, 0, "=2");
    set(book, id, 0, 1, "=A1*10");
    set(book, id, 1, 1, "=1/0");
    set(book, id, 0, 2, "=SUMIF(A1:A2,\">1\",B1:B2)");

    expect_error(evaluate(book, id, 0, 2), "#DIV/0!");
}

static void test_countif_escaped_wildcard_does_not_match_empty_text(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    set(book, id, 0, 0, "=\"\"");
    set(book, id, 1, 0, "*", true);
    set(book, id, 0, 1, "=COUNTIF(A1:A2,\"~*\")");

    expect_number(evaluate(book, id, 0, 1), 1);
}

static void test_cache_invalidation_and_cycles(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    set(book, id, 0, 0, "3");
    set(book, id, 0, 1, "=A1*2");
    expect_number(evaluate(book, id, 0, 1), 6);
    set(book, id, 0, 0, "4");
    expect_number(evaluate(book, id, 0, 1), 8);
    set(book, id, 1, 0, "=B2+1");
    set(book, id, 1, 1, "=A2+1");
    expect_error(evaluate(book, id, 1, 0), "#CYCLE!");
}

static void test_translation_and_structural_reference_rewrites(void)
{
    Text output;
    assert(translate_formula(output,
        "=A1+$B2+C$3+$D$4&\" A1 and $B2 \"\"text\"\"\"", 2, 3));
    assert(output == "=D3+$B4+F$3+$D$4&\" A1 and $B2 \"\"text\"\"\"");
    assert(translate_formula(output, "=A1", -1, 0));
    assert(output == "=#REF!");
    assert(translate_formula(output, "='任意 页'!$A1+Sheet!B$2+LOG10(100)", 1, 1));
    assert(output == "='任意 页'!$A2+Sheet!C$2+LOG10(100)");

    Workbook book;
    assert(book.init());
    uint32_t data = book.active_id;
    uint32_t formulas = add_sheet(book, "结果");
    set(book, formulas, 0, 0,
        "=Data!$A$3+Data!B$3+SUM(Data!A1:A4)&\"Data!A3\"");
    Sheet *data_sheet = book.sheet(data);
    assert(data_sheet);
    assert(data_sheet->name.set("Data"));
    assert(rewrite_axis(book, data, true, 2, 1, false));
    const Cell *changed = book.get(formulas, 0, 0);
    assert(changed);
    assert(changed->input == "=Data!$A$4+Data!B$4+SUM(Data!A1:A5)&\"Data!A3\"");

    set(book, formulas, 0, 1, "=LOG10(A1)+Data!A1");
    assert(rewrite_axis(book, data, true, 0, 1, false));
    changed = book.get(formulas, 0, 1);
    assert(changed);
    assert(changed->input == "=LOG10(A1)+Data!A2");

    set(book, formulas, 1, 0, "=SUM(Data!A1:A5)+Data!$B$4");
    assert(rewrite_axis(book, data, true, 1, 2, true));
    changed = book.get(formulas, 1, 0);
    assert(changed);
    assert(changed->input == "=SUM(Data!A1:A3)+Data!$B$2");

    set(book, formulas, 2, 0, "=SUM(Data!A3:A4)");
    assert(rewrite_axis(book, data, true, 2, 2, true));
    changed = book.get(formulas, 2, 0);
    assert(changed);
    assert(changed->input == "=SUM(#REF!)");
    expect_error(evaluate(book, formulas, 2, 0), "#REF!");
}

static void test_quote_safe_sheet_rename_and_delete(void)
{
    Workbook book;
    assert(book.init());
    uint32_t source = add_sheet(book, "旧 名称");
    uint32_t formulas = book.active_id;
    set(book, source, 0, 0, "9");
    set(book, formulas, 0, 0,
        "='旧 名称'!A1&\"旧 名称!A1\"&SUM('旧 名称'!A1:A1)");
    assert(rewrite_sheet_name(book, "旧 名称", "新 名称"));
    const Cell *changed = book.get(formulas, 0, 0);
    assert(changed);
    assert(changed->input ==
        "='新 名称'!A1&\"旧 名称!A1\"&SUM('新 名称'!A1:A1)");
    set(book, formulas, 1, 0, "=SUM('新 名称'!A1:A1)");
    assert(rewrite_sheet_name(book, "新 名称", nullptr));
    changed = book.get(formulas, 1, 0);
    assert(changed);
    assert(changed->input == "=SUM(#REF!)");
    expect_error(evaluate(book, formulas, 1, 0), "#REF!");
}

struct ProgressState { unsigned calls; bool cancel; };

static bool progress(void *context, unsigned completed, unsigned total)
{
    ProgressState *state = static_cast<ProgressState *>(context);
    assert(completed <= total);
    ++state->calls;
    return !state->cancel;
}

static void test_recalculate_progress_and_long_dependency_chain(void)
{
    Workbook book;
    assert(book.init());
    uint32_t id = book.active_id;
    char formula[40];
    for (int row = 0; row < MaxRows; ++row) {
        if (row == 0) set(book, id, row, 0, "1");
        else {
            snprintf(formula, sizeof(formula), "=A%d+1", row);
            set(book, id, row, 0, formula);
        }
    }
    expect_number(evaluate(book, id, MaxRows - 1, 0), MaxRows);
    book.invalidate();
    ProgressState state = { 0, true };
    assert(!recalculate(book, progress, &state));
    assert(state.calls > 0);
    state.calls = 0;
    state.cancel = false;
    assert(recalculate(book, progress, &state));
    assert(state.calls > 0);
    expect_number(evaluate(book, id, MaxRows - 1, 0), MaxRows);
}

int main(void)
{
    test_operators_literals_and_errors();
    test_sumif_uses_evaluated_sum_range_values();
    test_sumif_propagates_errors_from_sum_range();
    test_countif_escaped_wildcard_does_not_match_empty_text();
    test_references_functions_and_lazy_if();
    test_cache_invalidation_and_cycles();
    test_translation_and_structural_reference_rewrites();
    test_quote_safe_sheet_rename_and_delete();
    test_recalculate_progress_and_long_dependency_chain();
    puts("ok - ReliefExcel formula evaluation, functions, caches and references");
    return 0;
}
