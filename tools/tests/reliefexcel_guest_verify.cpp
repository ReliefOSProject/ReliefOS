#include "document.h"
#include "formula.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void check(bool condition, const char *message) {
    if (!condition) fail(message);
}

const rx::Sheet *sheet_at(const rx::Workbook &book, size_t index,
                          const char *description) {
    if (index >= book.sheets.size()) {
        char message[192];
        std::snprintf(message, sizeof(message), "%s is missing (found %zu sheet(s))",
                      description, book.sheets.size());
        fail(message);
        return nullptr;
    }
    return &book.sheets[index];
}

void check_sheet_name(const rx::Sheet *sheet, const char *expected,
                      const char *description) {
    if (!sheet) return;
    if (std::strcmp(sheet->name.c_str(), expected) == 0) return;
    char message[256];
    std::snprintf(message, sizeof(message), "%s: expected sheet name \"%s\", got \"%s\"",
                  description, expected, sheet->name.c_str());
    fail(message);
}

const rx::Cell *cell_at(const rx::Sheet *sheet, int row, int column,
                        const char *description) {
    if (!sheet) return nullptr;
    const rx::Cell *cell = sheet->get(row, column);
    if (cell) return cell;
    char message[256];
    std::snprintf(message, sizeof(message), "%s: cell at row %d, column %d is missing",
                  description, row + 1, column + 1);
    fail(message);
    return nullptr;
}

void check_input(const rx::Sheet *sheet, int row, int column,
                 const char *expected, const char *description) {
    const rx::Cell *cell = cell_at(sheet, row, column, description);
    if (!cell) return;
    if (std::strcmp(cell->input.c_str(), expected) == 0) return;
    char message[512];
    std::snprintf(message, sizeof(message), "%s: expected input \"%s\", got \"%s\"",
                  description, expected, cell->input.c_str());
    fail(message);
}

void check_number(rx::Workbook &book, const rx::Sheet *sheet, int row, int column,
                 double expected, const char *description) {
    const rx::Cell *cell = cell_at(sheet, row, column, description);
    if (!cell) return;
    rx::Value value = rx::evaluate(book, sheet->id, row, column);
    if (value.kind != rx::ValueKind::Number) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s: expected number %.15g, got value kind %u",
                      description, expected, static_cast<unsigned>(value.kind));
        fail(message);
        return;
    }
    if (std::isfinite(value.number) && std::fabs(value.number - expected) <= 1e-8) return;
    char message[256];
    std::snprintf(message, sizeof(message), "%s: expected %.15g, got %.15g",
                  description, expected, value.number);
    fail(message);
}

void check_formula_number(rx::Workbook &book, const rx::Sheet *sheet, int row, int column,
                          const char *formula, double expected, const char *description) {
    const rx::Cell *cell = cell_at(sheet, row, column, description);
    if (!cell) return;
    if (std::strcmp(cell->input.c_str(), formula) != 0) {
        char message[512];
        std::snprintf(message, sizeof(message), "%s: expected formula \"%s\", got \"%s\"",
                      description, formula, cell->input.c_str());
        fail(message);
    }
    check_number(book, sheet, row, column, expected, description);
}

bool verify_gui(rx::Workbook &book) {
    check(book.sheets.size() >= 3, "GUI workbook must contain at least three sheets");
    if (book.sheets.size() < 3) return false;

    const rx::Sheet *ledger = sheet_at(book, 0, "first sheet");
    const rx::Sheet *summary = sheet_at(book, 1, "second sheet");
    const rx::Sheet *sorted = sheet_at(book, 2, "third sheet");
    check_sheet_name(ledger, "记账明细", "first sheet");
    check_sheet_name(summary, "汇总", "second sheet");

    check_input(summary, 11, 3, "中文输入框粘贴测试", "Summary D12 pasted text");
    check_formula_number(book, summary, 11, 4, "=SUM(1,2,3)", 6.0, "Summary E12");
    check_number(book, summary, 12, 4, 7.0, "Summary E13");
    check_formula_number(book, summary, 11, 5, "=$E$12+E12", 12.0, "Summary F12");
    check_formula_number(book, summary, 12, 5, "=$E$12+E13", 13.0, "Summary F13");

    check_input(sorted, 0, 0, "key", "sorted sheet A1 header");
    check_input(sorted, 0, 1, "label", "sorted sheet B1 header");
    check_input(sorted, 0, 2, "sentinel", "sorted sheet C1 header");
    check_number(book, sorted, 1, 0, 1.0, "sorted sheet A2");
    check_input(sorted, 1, 1, "a", "sorted sheet B2");
    check_number(book, sorted, 2, 0, 2.0, "sorted sheet A3");
    check_input(sorted, 2, 1, "b", "sorted sheet B3");
    check_number(book, sorted, 1, 2, 20.0, "sorted sheet C2 unchanged");
    check_number(book, sorted, 2, 2, 10.0, "sorted sheet C3 unchanged");
    if (sorted) {
        check(!sorted->filter_enabled, "third sheet filter must be cleared");
        check(sorted->filters.empty(), "third sheet must have no remaining filter terms");
    }
    (void)ledger;
    return failures == 0;
}

bool verify_scale(rx::Workbook &book, bool edited) {
    if (book.sheets.size() != 2) {
        char message[160];
        std::snprintf(message, sizeof(message), "scale workbook must contain exactly 2 sheets, got %zu",
                      book.sheets.size());
        fail(message);
        return false;
    }

    const rx::Sheet *data = sheet_at(book, 0, "Data sheet");
    const rx::Sheet *summary = sheet_at(book, 1, "Summary sheet");
    check_sheet_name(data, "Data", "first sheet");
    check_sheet_name(summary, "Summary", "second sheet");
    if (!data || !summary) return false;

    check(data->used_rows() == 10000, "Data sheet must have 10000 used rows");
    check(data->used_columns() == 20, "Data sheet must have 20 used columns");
    check(data->cells.size() == 200000, "Data sheet must contain 200000 cells");
    check(book.cell_count() == 200006, "scale workbook must contain 200006 cells total");
    check_input(data, 0, 0, edited ? "-1" : "0", "Data A1");
    check_number(book, data, 0, 0, edited ? -1.0 : 0.0, "Data A1 value");

    check_input(summary, 0, 0, "Total", "Summary A1 label");
    check_input(summary, 1, 0, "Average", "Summary A2 label");
    check_input(summary, 2, 0, "Count", "Summary A3 label");
    check_formula_number(book, summary, 0, 1, "=SUM(Data!A1:T10000)",
                         edited ? 19999899999.0 : 19999900000.0, "Summary total");
    check_formula_number(book, summary, 1, 1, "=AVERAGE(Data!A1:T10000)",
                         edited ? 99999.499995 : 99999.5, "Summary average");
    check_formula_number(book, summary, 2, 1, "=COUNT(Data!A1:T10000)",
                         200000.0, "Summary count");
    return failures == 0;
}

bool verify_cut(rx::Workbook &book) {
    check(book.sheets.size()==1,"cut workbook must have one sheet");
    const rx::Sheet *sheet=sheet_at(book,0,"cut sheet");
    if (!sheet) return false;
    check(!sheet->get(0,0) || sheet->get(0,0)->input.empty(),"cut A1 must be empty");
    check(!sheet->get(0,1) || sheet->get(0,1)->input.empty(),"cut B1 must be empty");
    check_input(sheet,1,0,"5","moved A2");
    check_formula_number(book,sheet,1,1,"=A2+1",6,"moved formula B2");
    check_formula_number(book,sheet,0,3,"=SUM(A2:B2)",11,"dependent D1");
    return failures==0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s PATH MODE\nModes: gui, gui-burst, scale, scale-edited, cut\n",
                     argc > 0 ? argv[0] : "reliefexcel_guest_verify");
        return 2;
    }

    bool gui = std::strcmp(argv[2], "gui") == 0;
    bool gui_burst = std::strcmp(argv[2], "gui-burst") == 0;
    bool scale = std::strcmp(argv[2], "scale") == 0;
    bool scale_edited = std::strcmp(argv[2], "scale-edited") == 0;
    bool cut = std::strcmp(argv[2], "cut") == 0;
    if (!gui && !gui_burst && !scale && !scale_edited && !cut) {
        std::fprintf(stderr, "Unknown mode \"%s\". Expected gui, gui-burst, scale, scale-edited, or cut.\n", argv[2]);
        return 2;
    }

    rx::Workbook book;
    rx::Text error;
    if (!rx::load_workbook(argv[1], book, error)) {
        std::fprintf(stderr, "FAIL: could not load \"%s\": %s\n", argv[1],
                     error.empty() ? "unknown document error" : error.c_str());
        return 1;
    }

    if (gui || gui_burst) {
        verify_gui(book);
        if (gui_burst) {
            const rx::Sheet *summary=sheet_at(book,1,"burst input sheet");
            for (int index=0;index<9;++index) check_number(book,summary,19+index,4,index+1,"continuous keyboard input");
        }
    }
    else if (cut) verify_cut(book);
    else verify_scale(book, scale_edited);
    if (failures) {
        std::fprintf(stderr, "Verification failed: %d invariant(s) did not match (%s).\n",
                     failures, argv[2]);
        return 1;
    }

    std::printf("Verified ReliefExcel workbook (%s): %zu sheet(s), %zu cell(s).\n",
                argv[2], book.sheets.size(), book.cell_count());
    return 0;
}
