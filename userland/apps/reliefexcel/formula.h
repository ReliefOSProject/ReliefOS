#ifndef RELIEFEXCEL_FORMULA_H
#define RELIEFEXCEL_FORMULA_H
#include "model.h"
namespace rx {
Value evaluate(Workbook &book, uint32_t sheet, int row, int column);
bool recalculate(Workbook &book, Progress progress = nullptr, void *context = nullptr);
// Copy transformation: quoted strings remain untouched, $ locks the relevant axis.
bool translate_formula(Text &output, const char *input, int row_delta, int column_delta);
// Structural edit: both relative and absolute references follow inserted/deleted cells.
bool rewrite_axis(Workbook &book, uint32_t sheet, bool rows, int first, int count, bool deleted);
bool rewrite_sheet_name(Workbook &book, const char *old_name, const char *new_name); // null new_name => #REF!
// Move-aware cut rewrite. Fully-contained references follow the moved cells;
// partial range overlaps set unsupported so callers can reject atomically.
bool rewrite_move_references(Workbook &book, uint32_t source_sheet, Range source,
                             uint32_t target_sheet, int target_row, int target_col,
                             bool &unsupported, Progress progress = nullptr,
                             void *context = nullptr);
}
#endif
