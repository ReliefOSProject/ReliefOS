#include "model.h"
#include "formula.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rx {
namespace {

constexpr size_t HistoryLimit = 64;

enum class CommandKind : uint8_t { Cells, Snapshot };

struct CellState {
    bool present = false;
    Cell cell;
};

struct Change {
    int row = 0, col = 0;
    CellState before, after;
};

struct SheetMeta {
    Array<Range> merges;
    Array<int> column_widths, row_heights;
    Array<FilterTerm> filters;
    Range filter_range;
    bool filter_enabled = false, filter_header = true;
    bool freeze_row = false, freeze_column = false, gridlines = true;
    int zoom = 100, cursor_row = 0, cursor_col = 0, scroll_row = 0, scroll_col = 0;
};

struct EditCommand {
    CommandKind kind = CommandKind::Cells;
    Array<CellState> before_cells, after_cells;
    uint32_t cell_sheet = 0;
    uint32_t meta_sheet = 0;
    bool has_meta = false;
    SheetMeta before_meta, after_meta;
    Workbook snapshot;
};

void set_error(Editor &editor, const char *message) {
    if (message) editor.error.set(message);
    else editor.error.clear();
}

bool report_progress(Editor &editor, unsigned done, unsigned total) {
    if (!editor.progress || editor.progress(editor.progress_context, done, total)) return true;
    set_error(editor, "操作已取消");
    return false;
}

CellState read_state(const Workbook &book, uint32_t sheet_id, int row, int col) {
    CellState state;
    state.cell.row = row;
    state.cell.col = col;
    const Cell *cell = book.get(sheet_id, row, col);
    if (cell) {
        state.present = true;
        state.cell = *cell;
    }
    return state;
}

bool same_cell_value(const CellState &a, const CellState &b) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    return a.cell.row == b.cell.row && a.cell.col == b.cell.col &&
        a.cell.input == b.cell.input && a.cell.style == b.cell.style && a.cell.literal == b.cell.literal;
}

bool make_input_state(const Workbook &book, uint32_t sheet_id, int row, int col,
                      const char *input, bool literal, CellState &state) {
    if (!input) return false;
    const size_t length = strlen(input);
    if (length > MaxTextBytes || !valid_utf8(input, length)) return false;
    state = read_state(book, sheet_id, row, col);
    if (!length) {
        state.cell.input.clear();
        state.cell.literal = false;
        state.cell.result = Value{};
        state.cell.calculated = 0;
        state.cell.evaluating = 0;
        if (state.cell.style == Style{}) state.present = false;
        return true;
    }
    Text text;
    if (!text.set(input, length)) return false;
    state.present = true;
    state.cell.row = row;
    state.cell.col = col;
    state.cell.input = static_cast<Text &&>(text);
    state.cell.literal = literal;
    state.cell.result = Value{};
    state.cell.calculated = 0;
    state.cell.evaluating = 0;
    return true;
}

bool make_clear_state(const Workbook &book, uint32_t sheet_id, int row, int col, CellState &state) {
    state = read_state(book, sheet_id, row, col);
    if (!state.present) return true;
    state.cell.input.clear();
    state.cell.literal = false;
    state.cell.result = Value{};
    state.cell.calculated = 0;
    state.cell.evaluating = 0;
    if (state.cell.style == Style{}) state.present = false;
    return true;
}

class MutationPlan {
    Workbook &book_;
    uint32_t sheet_;
    Array<Change> changes_;
    Array<uint32_t> index_;
    size_t final_count_;

    static uint32_t coordinate_key(int row, int col) {
        return static_cast<uint32_t>(row * MaxCols + col);
    }

    static size_t hash_key(uint32_t key, size_t mask) {
        uint32_t mixed = key;
        mixed ^= mixed >> 16;
        mixed *= 0x7feb352du;
        mixed ^= mixed >> 15;
        mixed *= 0x846ca68bu;
        mixed ^= mixed >> 16;
        return static_cast<size_t>(mixed) & mask;
    }

    bool find(int row, int col, size_t &change_index, size_t &slot) const {
        if (index_.empty()) return false;
        const uint32_t key = coordinate_key(row, col);
        const size_t mask = index_.size() - 1;
        slot = hash_key(key, mask);
        while (index_[slot]) {
            size_t candidate = static_cast<size_t>(index_[slot] - 1);
            if (changes_[candidate].row == row && changes_[candidate].col == col) {
                change_index = candidate;
                return true;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }

    bool rehash(size_t capacity) {
        Array<uint32_t> next;
        if (capacity < 16 || (capacity & (capacity - 1)) || !next.resize(capacity)) return false;
        const size_t mask = capacity - 1;
        for (size_t i = 0; i < changes_.size(); ++i) {
            size_t slot = hash_key(coordinate_key(changes_[i].row, changes_[i].col), mask);
            while (next[slot]) slot = (slot + 1) & mask;
            if (i >= UINT32_MAX) return false;
            next[slot] = static_cast<uint32_t>(i + 1);
        }
        index_.swap(next);
        return true;
    }

    bool prepare_insert() {
        size_t capacity = index_.size();
        if (!capacity) return rehash(16);
        if ((changes_.size() + 1) * 10 < capacity * 7) return true;
        if (capacity > SIZE_MAX / 2) return false;
        return rehash(capacity * 2);
    }

    static bool change_less(const Change &a, const Change &b) {
        return a.row < b.row || (a.row == b.row && a.col < b.col);
    }

    static void change_swap(Change &a, Change &b) {
        Change temporary = static_cast<Change &&>(a);
        a = static_cast<Change &&>(b);
        b = static_cast<Change &&>(temporary);
    }

    void sift_down(size_t root, size_t count) {
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= count) return;
            if (child + 1 < count && change_less(changes_[child], changes_[child + 1])) ++child;
            if (!change_less(changes_[root], changes_[child])) return;
            change_swap(changes_[root], changes_[child]);
            root = child;
        }
    }

    void sort_changes() {
        const size_t count = changes_.size();
        for (size_t start = count / 2; start > 0; --start) sift_down(start - 1, count);
        for (size_t end = count; end > 1; --end) {
            change_swap(changes_[0], changes_[end - 1]);
            sift_down(0, end - 1);
        }
    }
public:
    MutationPlan(Workbook &book, uint32_t sheet) : book_(book), sheet_(sheet), final_count_(book.cell_count()) {}
    size_t size() const { return changes_.size(); }
    uint32_t sheet() const { return sheet_; }
    const Change &operator[](size_t at) const { return changes_[at]; }
    Change &operator[](size_t at) { return changes_[at]; }

    bool set(int row, int col, const CellState &requested) {
        if (row < 0 || row >= MaxRows || col < 0 || col >= MaxCols) return false;
        CellState after = requested;
        after.cell.row = row;
        after.cell.col = col;
        size_t found_index = 0, slot = 0;
        if (find(row, col, found_index, slot)) {
            bool was_present = changes_[found_index].after.present;
            size_t next_count = final_count_;
            if (was_present && !after.present) --next_count;
            else if (!was_present && after.present) ++next_count;
            if (next_count > MaxCells) return false;
            changes_[found_index].after = static_cast<CellState &&>(after);
            final_count_ = next_count;
            return true;
        }
        if (!prepare_insert() || find(row, col, found_index, slot)) return false;
        Change change;
        change.row = row;
        change.col = col;
        change.before = read_state(book_, sheet_, row, col);
        change.after = static_cast<CellState &&>(after);
        size_t next_count = final_count_;
        if (change.before.present && !change.after.present) --next_count;
        else if (!change.before.present && change.after.present) ++next_count;
        if (next_count > MaxCells || changes_.size() >= UINT32_MAX ||
            !changes_.push(static_cast<Change &&>(change))) return false;
        index_[slot] = static_cast<uint32_t>(changes_.size());
        final_count_ = next_count;
        return true;
    }

    bool set_current(int row, int col) {
        CellState state = read_state(book_, sheet_, row, col);
        return set(row, col, state);
    }

    void compact() {
        size_t kept = 0;
        for (size_t i = 0; i < changes_.size(); ++i) {
            if (same_cell_value(changes_[i].before, changes_[i].after)) continue;
            if (kept != i) changes_[kept] = static_cast<Change &&>(changes_[i]);
            ++kept;
        }
        changes_.resize(kept);
        index_.reset();
        sort_changes();
    }
};

bool clone_meta(const Sheet &sheet, SheetMeta &meta) {
    Array<Range> merges;
    Array<int> widths, heights;
    Array<FilterTerm> filters;
    if (!merges.clone_from(sheet.merges) || !widths.clone_from(sheet.column_widths) ||
        !heights.clone_from(sheet.row_heights) || !filters.clone_from(sheet.filters)) return false;
    meta.merges.swap(merges);
    meta.column_widths.swap(widths);
    meta.row_heights.swap(heights);
    meta.filters.swap(filters);
    meta.filter_range = sheet.filter_range;
    meta.filter_enabled = sheet.filter_enabled;
    meta.filter_header = sheet.filter_header;
    meta.freeze_row = sheet.freeze_row;
    meta.freeze_column = sheet.freeze_column;
    meta.gridlines = sheet.gridlines;
    meta.zoom = sheet.zoom;
    meta.cursor_row = sheet.cursor_row;
    meta.cursor_col = sheet.cursor_col;
    meta.scroll_row = sheet.scroll_row;
    meta.scroll_col = sheet.scroll_col;
    return true;
}

bool clone_meta(const SheetMeta &source, SheetMeta &meta) {
    Array<Range> merges;
    Array<int> widths, heights;
    Array<FilterTerm> filters;
    if (!merges.clone_from(source.merges) || !widths.clone_from(source.column_widths) ||
        !heights.clone_from(source.row_heights) || !filters.clone_from(source.filters)) return false;
    meta.merges.swap(merges);
    meta.column_widths.swap(widths);
    meta.row_heights.swap(heights);
    meta.filters.swap(filters);
    meta.filter_range = source.filter_range;
    meta.filter_enabled = source.filter_enabled;
    meta.filter_header = source.filter_header;
    meta.freeze_row = source.freeze_row;
    meta.freeze_column = source.freeze_column;
    meta.gridlines = source.gridlines;
    meta.zoom = source.zoom;
    meta.cursor_row = source.cursor_row;
    meta.cursor_col = source.cursor_col;
    meta.scroll_row = source.scroll_row;
    meta.scroll_col = source.scroll_col;
    return true;
}

bool equal_meta(const SheetMeta &a, const SheetMeta &b) {
    if (!(a.filter_range.r1 == b.filter_range.r1 && a.filter_range.c1 == b.filter_range.c1 &&
          a.filter_range.r2 == b.filter_range.r2 && a.filter_range.c2 == b.filter_range.c2) ||
        a.filter_enabled != b.filter_enabled || a.filter_header != b.filter_header ||
        a.freeze_row != b.freeze_row || a.freeze_column != b.freeze_column ||
        a.gridlines != b.gridlines || a.zoom != b.zoom || a.cursor_row != b.cursor_row ||
        a.cursor_col != b.cursor_col || a.scroll_row != b.scroll_row || a.scroll_col != b.scroll_col ||
        a.merges.size() != b.merges.size() || a.column_widths.size() != b.column_widths.size() ||
        a.row_heights.size() != b.row_heights.size() || a.filters.size() != b.filters.size()) return false;
    for (size_t i = 0; i < a.merges.size(); ++i) {
        if (a.merges[i].r1 != b.merges[i].r1 || a.merges[i].c1 != b.merges[i].c1 ||
            a.merges[i].r2 != b.merges[i].r2 || a.merges[i].c2 != b.merges[i].c2) return false;
    }
    for (size_t i = 0; i < a.column_widths.size(); ++i) if (a.column_widths[i] != b.column_widths[i]) return false;
    for (size_t i = 0; i < a.row_heights.size(); ++i) if (a.row_heights[i] != b.row_heights[i]) return false;
    for (size_t i = 0; i < a.filters.size(); ++i)
        if (a.filters[i].column != b.filters[i].column || a.filters[i].operation != b.filters[i].operation ||
            !(a.filters[i].query == b.filters[i].query)) return false;
    return true;
}

void install_meta(Sheet &sheet, SheetMeta &&meta) {
    sheet.merges.swap(meta.merges);
    sheet.column_widths.swap(meta.column_widths);
    sheet.row_heights.swap(meta.row_heights);
    sheet.filters.swap(meta.filters);
    sheet.filter_range = meta.filter_range;
    sheet.filter_enabled = meta.filter_enabled;
    sheet.filter_header = meta.filter_header;
    sheet.freeze_row = meta.freeze_row;
    sheet.freeze_column = meta.freeze_column;
    sheet.gridlines = meta.gridlines;
    sheet.zoom = meta.zoom;
    sheet.cursor_row = meta.cursor_row;
    sheet.cursor_col = meta.cursor_col;
    sheet.scroll_row = meta.scroll_row;
    sheet.scroll_col = meta.scroll_col;
}

bool apply_cells(Workbook &book, uint32_t sheet_id, const Array<CellState> &states) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet) return false;
    const Array<Cell> &cells = sheet->cells;
    size_t additions = 0, deletions = 0;
    size_t cell_at = 0, state_at = 0;
    int previous_row = -1, previous_col = -1;
    for (size_t i = 0; i < states.size(); ++i) {
        const CellState &state = states[i];
        if (state.cell.row < 0 || state.cell.row >= MaxRows ||
            state.cell.col < 0 || state.cell.col >= MaxCols ||
            state.cell.row < previous_row ||
            (state.cell.row == previous_row && state.cell.col <= previous_col)) return false;
        previous_row = state.cell.row;
        previous_col = state.cell.col;
    }
    while (cell_at < cells.size() || state_at < states.size()) {
        const CellState *state = state_at < states.size() ? &states[state_at] : nullptr;
        const Cell *cell = cell_at < cells.size() ? &cells[cell_at] : nullptr;

        bool state_first = state && (!cell || state->cell.row < cell->row ||
            (state->cell.row == cell->row && state->cell.col < cell->col));
        bool same = state && cell && state->cell.row == cell->row && state->cell.col == cell->col;
        if (state_first) {
            if (state->present) ++additions;
            ++state_at;
        } else if (same) {
            if (!state->present) ++deletions;
            ++state_at;
            ++cell_at;
        } else {
            ++cell_at;
        }
    }
    const size_t workbook_cells = book.cell_count();
    if (deletions > workbook_cells || additions > MaxCells - (workbook_cells - deletions)) return false;
    const size_t final_sheet_count = cells.size() - deletions + additions;
    Array<Cell> merged;
    if (!merged.reserve(final_sheet_count)) return false;

    cell_at = 0;
    state_at = 0;
    while (cell_at < cells.size() || state_at < states.size()) {
        const CellState *state = state_at < states.size() ? &states[state_at] : nullptr;
        const Cell *cell = cell_at < cells.size() ? &cells[cell_at] : nullptr;
        bool state_first = state && (!cell || state->cell.row < cell->row ||
            (state->cell.row == cell->row && state->cell.col < cell->col));
        bool same = state && cell && state->cell.row == cell->row && state->cell.col == cell->col;
        if (state_first) {
            if (state->present && !merged.push(state->cell)) return false;
            ++state_at;
        } else if (same) {
            if (state->present && !merged.push(state->cell)) return false;
            ++state_at;
            ++cell_at;
        } else {
            if (!merged.push(*cell)) return false;
            ++cell_at;
        }
    }
    if (merged.size() != final_sheet_count) return false;
    sheet->cells.swap(merged);
    return true;
}

template<class HistoryType>
bool commit_plan(Editor &editor, HistoryType *history, MutationPlan &plan,
                 uint32_t meta_sheet = 0, const SheetMeta *before_meta = nullptr,
                 SheetMeta *after_meta = nullptr) {
    plan.compact();
    if (!plan.size() && !meta_sheet) return true;
    Sheet *target = meta_sheet ? editor.book.sheet(meta_sheet) : nullptr;
    if (meta_sheet && (!target || !before_meta || !after_meta)) {
        set_error(editor, "工作表已不存在");
        return false;
    }
    if (meta_sheet && equal_meta(*before_meta, *after_meta)) meta_sheet = 0;
    if (!plan.size() && !meta_sheet) return true;

    EditCommand command;
    Array<CellState> no_history_states;
    Array<CellState> *states = &no_history_states;
    SheetMeta staged_meta;
    if (history) {
        command.cell_sheet = plan.sheet();
        if (history->undo.size() >= HistoryLimit) {
            // Erasing the oldest command is deferred until every allocation succeeds.
        }
        if (!history->undo.reserve(history->undo.size() + 1) ||
            !command.before_cells.reserve(plan.size()) || !command.after_cells.reserve(plan.size())) {
            set_error(editor, "内存不足，操作未执行");
            return false;
        }
        for (size_t i = 0; i < plan.size(); ++i) {
            if (!command.before_cells.push(plan[i].before) || !command.after_cells.push(plan[i].after)) {
                set_error(editor, "内存不足，操作未执行");
                return false;
            }
        }
        if (meta_sheet) {
            command.has_meta = true;
            command.meta_sheet = meta_sheet;
            if (!command.before_meta.merges.clone_from(before_meta->merges) ||
                !command.before_meta.column_widths.clone_from(before_meta->column_widths) ||
                !command.before_meta.row_heights.clone_from(before_meta->row_heights) ||
                !command.before_meta.filters.clone_from(before_meta->filters) ||
                !command.after_meta.merges.clone_from(after_meta->merges) ||
                !command.after_meta.column_widths.clone_from(after_meta->column_widths) ||
                !command.after_meta.row_heights.clone_from(after_meta->row_heights) ||
                !command.after_meta.filters.clone_from(after_meta->filters)) {
                set_error(editor, "内存不足，操作未执行");
                return false;
            }
            command.before_meta.filter_range = before_meta->filter_range;
            command.before_meta.filter_enabled = before_meta->filter_enabled;
            command.before_meta.filter_header = before_meta->filter_header;
            command.before_meta.freeze_row = before_meta->freeze_row;
            command.before_meta.freeze_column = before_meta->freeze_column;
            command.before_meta.gridlines = before_meta->gridlines;
            command.before_meta.zoom = before_meta->zoom;
            command.before_meta.cursor_row = before_meta->cursor_row;
            command.before_meta.cursor_col = before_meta->cursor_col;
            command.before_meta.scroll_row = before_meta->scroll_row;
            command.before_meta.scroll_col = before_meta->scroll_col;
            command.after_meta.filter_range = after_meta->filter_range;
            command.after_meta.filter_enabled = after_meta->filter_enabled;
            command.after_meta.filter_header = after_meta->filter_header;
            command.after_meta.freeze_row = after_meta->freeze_row;
            command.after_meta.freeze_column = after_meta->freeze_column;
            command.after_meta.gridlines = after_meta->gridlines;
            command.after_meta.zoom = after_meta->zoom;
            command.after_meta.cursor_row = after_meta->cursor_row;
            command.after_meta.cursor_col = after_meta->cursor_col;
            command.after_meta.scroll_row = after_meta->scroll_row;
            command.after_meta.scroll_col = after_meta->scroll_col;
            if (!clone_meta(*target, staged_meta)) {
                set_error(editor, "内存不足，操作未执行");
                return false;
            }
            // staged_meta currently contains the old state; replace its arrays with the new state.
            SheetMeta new_staged;
            if (!new_staged.merges.clone_from(after_meta->merges) ||
                !new_staged.column_widths.clone_from(after_meta->column_widths) ||
                !new_staged.row_heights.clone_from(after_meta->row_heights) ||
                !new_staged.filters.clone_from(after_meta->filters)) {
                set_error(editor, "内存不足，操作未执行");
                return false;
            }
            new_staged.filter_range = after_meta->filter_range;
            new_staged.filter_enabled = after_meta->filter_enabled;
            new_staged.filter_header = after_meta->filter_header;
            new_staged.freeze_row = after_meta->freeze_row;
            new_staged.freeze_column = after_meta->freeze_column;
            new_staged.gridlines = after_meta->gridlines;
            new_staged.zoom = after_meta->zoom;
            new_staged.cursor_row = after_meta->cursor_row;
            new_staged.cursor_col = after_meta->cursor_col;
            new_staged.scroll_row = after_meta->scroll_row;
            new_staged.scroll_col = after_meta->scroll_col;
            staged_meta = static_cast<SheetMeta &&>(new_staged);
        }
        states = &command.after_cells;
    } else {
        if (!no_history_states.reserve(plan.size())) {
            set_error(editor, "内存不足，操作未执行");
            return false;
        }
        for (size_t i = 0; i < plan.size(); ++i) if (!no_history_states.push(plan[i].after)) {
            set_error(editor, "内存不足，操作未执行");
            return false;
        }
        if (meta_sheet) staged_meta = static_cast<SheetMeta &&>(*after_meta);
    }

    if (!apply_cells(editor.book, plan.sheet(), *states)) {
        set_error(editor, "操作范围无效或超出工作簿限制");
        return false;
    }
    if (meta_sheet) {
        target = editor.book.sheet(meta_sheet);
        install_meta(*target, static_cast<SheetMeta &&>(staged_meta));
    }
    editor.book.invalidate();
    if (history) {
        if (history->undo.size() >= HistoryLimit) history->undo.erase(0);
        history->redo.clear();
        history->undo.push(static_cast<EditCommand &&>(command));
    }
    set_error(editor, nullptr);
    return true;
}

void swap_workbooks(Workbook &a, Workbook &b) {
    a.sheets.swap(b.sheets);
    uint32_t value = a.next_id; a.next_id = b.next_id; b.next_id = value;
    value = a.active_id; a.active_id = b.active_id; b.active_id = value;
    uint64_t revision = a.revision; a.revision = b.revision; b.revision = revision;
}

template<class HistoryType>
bool commit_snapshot(Editor &editor, HistoryType *history, Workbook &&candidate) {
    if (history && !history->undo.reserve(history->undo.size() + 1)) {
        set_error(editor, "内存不足，操作未执行");
        return false;
    }
    candidate.invalidate();
    if (history) {
        EditCommand command;
        command.kind = CommandKind::Snapshot;
        command.snapshot = static_cast<Workbook &&>(editor.book);
        editor.book = static_cast<Workbook &&>(candidate);
        if (history->undo.size() >= HistoryLimit) history->undo.erase(0);
        history->redo.clear();
        history->undo.push(static_cast<EditCommand &&>(command));
    } else {
        editor.book = static_cast<Workbook &&>(candidate);
    }
    set_error(editor, nullptr);
    return true;
}

bool range_overlaps(const Range &a, const Range &b) {
    return a.r1 <= b.r2 && b.r1 <= a.r2 && a.c1 <= b.c2 && b.c1 <= a.c2;
}

bool ranges_equal(const Range &a, const Range &b) {
    return a.r1 == b.r1 && a.c1 == b.c1 && a.r2 == b.r2 && a.c2 == b.c2;
}

bool replace_text(const char *input, const char *find, const char *replacement,
                  bool match_case, Text &output, unsigned &replaced) {
    const size_t input_len = strlen(input), find_len = strlen(find), replacement_len = strlen(replacement);
    if (!find_len) return false;
    size_t matches = 0;
    for (size_t i = 0; i + find_len <= input_len;) {
        bool equal = true;
        for (size_t j = 0; j < find_len; ++j) {
            unsigned char a = static_cast<unsigned char>(input[i + j]);
            unsigned char b = static_cast<unsigned char>(find[j]);
            if (!match_case && a < 0x80) a = static_cast<unsigned char>(tolower(a));
            if (!match_case && b < 0x80) b = static_cast<unsigned char>(tolower(b));
            if (a != b) { equal = false; break; }
        }
        if (equal) { ++matches; i += find_len; }
        else ++i;
    }
    if (!matches) { replaced = 0; return output.set(input, input_len); }
    if (matches > (SIZE_MAX - input_len) / (replacement_len > find_len ? replacement_len - find_len : 1)) return false;
    size_t output_len = input_len;
    if (replacement_len >= find_len) output_len += matches * (replacement_len - find_len);
    else output_len -= matches * (find_len - replacement_len);
    if (output_len > MaxTextBytes) return false;
    char *buffer = static_cast<char *>(malloc(output_len + 1));
    if (!buffer) return false;
    size_t in = 0, out = 0;
    while (in < input_len) {
        bool equal = in + find_len <= input_len;
        for (size_t j = 0; equal && j < find_len; ++j) {
            unsigned char a = static_cast<unsigned char>(input[in + j]);
            unsigned char b = static_cast<unsigned char>(find[j]);
            if (!match_case && a < 0x80) a = static_cast<unsigned char>(tolower(a));
            if (!match_case && b < 0x80) b = static_cast<unsigned char>(tolower(b));
            if (a != b) equal = false;
        }
        if (equal) {
            memcpy(buffer + out, replacement, replacement_len);
            out += replacement_len;
            in += find_len;
        } else buffer[out++] = input[in++];
    }
    buffer[out] = 0;
    bool ok = output.set(buffer, output_len);
    free(buffer);
    if (ok) replaced = static_cast<unsigned>(matches);
    return ok;
}

int compare_text(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char x = static_cast<unsigned char>(*a++), y = static_cast<unsigned char>(*b++);
        if (x < 0x80) x = static_cast<unsigned char>(tolower(x));
        if (y < 0x80) y = static_cast<unsigned char>(tolower(y));
        if (x != y) return x < y ? -1 : 1;
    }
    return *a ? 1 : (*b ? -1 : 0);
}

int compare_values(const Value &a, const Value &b) {
    if (a.kind != b.kind) return static_cast<unsigned>(a.kind) < static_cast<unsigned>(b.kind) ? -1 : 1;
    switch (a.kind) {
    case ValueKind::Empty: return 0;
    case ValueKind::Number: return a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
    case ValueKind::Text:
    case ValueKind::Boolean:
    case ValueKind::Error: return compare_text(a.text.c_str(), b.text.c_str());
    }
    return 0;
}

bool adjust_range_axis(Range &range, bool rows, int first, int count, bool deleted) {
    int &start = rows ? range.r1 : range.c1;
    int &end = rows ? range.r2 : range.c2;
    if (!deleted) {
        if (start >= first) { start += count; end += count; }
        else if (end >= first) end += count;
        return end < (rows ? MaxRows : MaxCols);
    }
    int last = first + count - 1;
    if (end < first) return true;
    if (start > last) { start -= count; end -= count; return true; }
    int old_start = start, old_end = end;
    int removed_start = old_start > first ? old_start : first;
    int removed_end = old_end < last ? old_end : last;
    int removed = removed_end - removed_start + 1;
    int remaining = old_end - old_start + 1 - removed;
    if (remaining <= 0) return false;
    start = old_start < first ? old_start : first;
    end = start + remaining - 1;
    return true;
}

bool adjust_dimensions(Array<int> &values, int first, int count, bool deleted, int limit) {
    if (values.empty()) return true;
    if (!deleted) {
        if (first >= static_cast<int>(values.size())) return true;
        if (values.size() + static_cast<size_t>(count) > static_cast<size_t>(limit)) return false;
        if (static_cast<size_t>(count) > SIZE_MAX - values.size()) return false;
        if (!values.reserve(values.size() + static_cast<size_t>(count))) return false;
        for (int i = 0; i < count; ++i) if (!values.insert(static_cast<size_t>(first), 0)) return false;
        return true;
    }
    if (first >= static_cast<int>(values.size())) return true;
    size_t amount = static_cast<size_t>(count);
    if (amount > values.size() - static_cast<size_t>(first)) amount = values.size() - static_cast<size_t>(first);
    values.erase(static_cast<size_t>(first), amount);
    return true;
}

bool adjust_sheet_for_axis(Sheet &sheet, bool rows, int first, int count, bool deleted) {
    const int end = first + count - 1;
    for (size_t i = 0; i < sheet.merges.size();) {
        Range &merge = sheet.merges[i];
        int low = rows ? merge.r1 : merge.c1;
        int high = rows ? merge.r2 : merge.c2;
        if (!deleted) {
            if (first > low && first <= high) return false;
            if (first <= low) {
                if (rows) { merge.r1 += count; merge.r2 += count; }
                else { merge.c1 += count; merge.c2 += count; }
            }
            if ((rows ? merge.r2 : merge.c2) >= (rows ? MaxRows : MaxCols)) return false;
            ++i;
            continue;
        }
        bool overlaps = low <= end && first <= high;
        if (overlaps && !(first <= low && end >= high)) return false;
        if (overlaps) { sheet.merges.erase(i); continue; }
        if (low > end) {
            if (rows) { merge.r1 -= count; merge.r2 -= count; }
            else { merge.c1 -= count; merge.c2 -= count; }
        }
        ++i;
    }
    if (rows) {
        if (!adjust_dimensions(sheet.row_heights, first, count, deleted, MaxRows)) return false;
    } else {
        if (!adjust_dimensions(sheet.column_widths, first, count, deleted, MaxCols)) return false;
    }
    if (sheet.filter_enabled) {
        bool valid = adjust_range_axis(sheet.filter_range, rows, first, count, deleted);
        if (!valid) { sheet.filter_enabled = false; sheet.filters.clear(); }
        if (!rows) {
            for (size_t i = 0; i < sheet.filters.size();) {
                FilterTerm &term = sheet.filters[i];
                if (!deleted && term.column >= first) term.column += count;
                else if (deleted && term.column >= first && term.column <= end) { sheet.filters.erase(i); continue; }
                else if (deleted && term.column > end) term.column -= count;
                ++i;
            }
        }
    }
    int &cursor = rows ? sheet.cursor_row : sheet.cursor_col;
    int &scroll = rows ? sheet.scroll_row : sheet.scroll_col;
    if (!deleted) {
        if (cursor >= first) cursor += count;
        if (scroll >= first) scroll += count;
        if (cursor >= (rows ? MaxRows : MaxCols) || scroll >= (rows ? MaxRows : MaxCols)) return false;
    } else {
        if (cursor >= first && cursor <= end) cursor = first < (rows ? MaxRows : MaxCols) ? first : (rows ? MaxRows : MaxCols) - 1;
        else if (cursor > end) cursor -= count;
        if (scroll >= first && scroll <= end) scroll = first;
        else if (scroll > end) scroll -= count;
    }
    return true;
}

} // namespace

struct Editor::History {
    Array<EditCommand> undo, redo;
};

Editor::Editor() {
    book.init();
    void *memory = malloc(sizeof(History));
    if (memory) history_ = new (memory, Placement{}) History;
}

Editor::~Editor() {
    if (history_) {
        history_->~History();
        free(history_);
        history_ = nullptr;
    }
}

bool Editor::set_cell(uint32_t sheet_id, int row, int col, const char *input, bool literal) {
    if (!book.sheet(sheet_id) || row < 0 || row >= MaxRows || col < 0 || col >= MaxCols || !input) {
        set_error(*this, "单元格地址无效");
        return false;
    }
    CellState after;
    if (!make_input_state(book, sheet_id, row, col, input, literal, after)) {
        set_error(*this, "文本无效、过长或内存不足");
        return false;
    }
    CellState before = read_state(book, sheet_id, row, col);
    if (same_cell_value(before, after)) { set_error(*this, nullptr); return true; }
    MutationPlan plan(book, sheet_id);
    if (!plan.set(row, col, after)) {
        set_error(*this, "内存不足，操作未执行");
        return false;
    }
    return commit_plan(*this, history_, plan);
}

bool Editor::style_range(uint32_t sheet_id, Range range, const Style &style) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !range.valid()) { set_error(*this, "格式范围无效"); return false; }
    MutationPlan plan(book, sheet_id);
    if (!(style == Style{})) {
        size_t area = static_cast<size_t>(range.r2 - range.r1 + 1) *
            static_cast<size_t>(range.c2 - range.c1 + 1);
        size_t present = 0;
        for (size_t i = 0; i < sheet->cells.size(); ++i)
            if (range.contains(sheet->cells[i].row, sheet->cells[i].col)) ++present;
        if (book.cell_count() - present + area > MaxCells) {
            set_error(*this, "格式范围超过工作簿单元格上限");
            return false;
        }
        const unsigned total = static_cast<unsigned>(range.r2 - range.r1 + 1);
        if (!report_progress(*this, 0, total)) return false;
        for (int row = range.r1; row <= range.r2; ++row) {
            for (int col = range.c1; col <= range.c2; ++col) {
                const Cell *current = sheet->get(row, col);
                if (current && current->style == style) continue;
                CellState after = read_state(book, sheet_id, row, col);
                after.present = true;
                after.cell.row = row;
                after.cell.col = col;
                after.cell.style = style;
                after.cell.result = Value{};
                after.cell.calculated = 0;
                after.cell.evaluating = 0;
                if (!plan.set(row, col, after)) { set_error(*this, "工作簿单元格已达上限或内存不足"); return false; }
            }
            if (!report_progress(*this, static_cast<unsigned>(row - range.r1 + 1), total)) return false;
        }
    } else {
        const size_t total = sheet->cells.size();
        if (!report_progress(*this, 0, static_cast<unsigned>(total))) return false;
        for (size_t i = 0; i < sheet->cells.size(); ++i) {
            const Cell &cell = sheet->cells[i];
            if (!range.contains(cell.row, cell.col) || cell.style == style) continue;
            CellState after = read_state(book, sheet_id, cell.row, cell.col);
            after.cell.style = style;
            if (after.cell.input.empty()) after.present = false;
            if (!plan.set(cell.row, cell.col, after)) { set_error(*this, "内存不足，操作未执行"); return false; }
            if ((i & 255u) == 255u && !report_progress(*this, static_cast<unsigned>(i + 1), static_cast<unsigned>(total))) return false;
        }
        if (!report_progress(*this, static_cast<unsigned>(total), static_cast<unsigned>(total))) return false;
    }
    return commit_plan(*this, history_, plan);
}

bool Editor::patch_style(uint32_t sheet_id, Range range, const Style &patch, uint32_t mask) {
    Sheet *sheet = book.sheet(sheet_id);
    constexpr uint32_t known_mask = 1u | 2u | 4u | 8u | 16u | 32u | 64u | 128u | 256u;
    if (!sheet || !range.valid() || (mask & ~known_mask)) {
        set_error(*this, "格式范围或属性掩码无效");
        return false;
    }
    if (!mask) { set_error(*this, nullptr); return true; }

    MutationPlan plan(book, sheet_id);
    const unsigned total = static_cast<unsigned>(range.r2 - range.r1 + 1);
    if (!report_progress(*this, 0, total)) return false;
    for (int row = range.r1; row <= range.r2; ++row) {
        for (int col = range.c1; col <= range.c2; ++col) {
            const Cell *current = sheet->get(row, col);
            Style next = current ? current->style : Style{};
            if (mask & 1u) next.foreground = patch.foreground;
            if (mask & 2u) next.background = patch.background;
            if (mask & 4u) next.size = patch.size;
            if (mask & 8u) next.bold = patch.bold ? 1 : 0;
            if (mask & 16u) next.italic = patch.italic ? 1 : 0;
            if (mask & 32u) next.wrap = patch.wrap ? 1 : 0;
            if (mask & 64u) next.borders = patch.borders;
            if (mask & 128u) next.alignment = patch.alignment;
            if (mask & 256u) next.format = patch.format;
            if (current && current->style == next) continue;

            CellState after = read_state(book, sheet_id, row, col);
            after.cell.style = next;
            if (after.cell.input.empty() && next == Style{}) after.present = false;
            else after.present = true;
            if (!plan.set(row, col, after)) {
                set_error(*this, "工作簿单元格已达上限或内存不足");
                return false;
            }
        }
        if (!report_progress(*this, static_cast<unsigned>(row - range.r1 + 1), total)) return false;
    }
    return commit_plan(*this, history_, plan);
}

bool Editor::clear_range(uint32_t sheet_id, Range range) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !range.valid()) { set_error(*this, "清除范围无效"); return false; }
    MutationPlan plan(book, sheet_id);
    const size_t total = sheet->cells.size();
    if (!report_progress(*this, 0, static_cast<unsigned>(total))) return false;
    for (size_t i = 0; i < sheet->cells.size(); ++i) {
        const Cell &cell = sheet->cells[i];
        if (!range.contains(cell.row, cell.col)) continue;
        CellState after;
        make_clear_state(book, sheet_id, cell.row, cell.col, after);
        if (!plan.set(cell.row, cell.col, after)) { set_error(*this, "内存不足，操作未执行"); return false; }
        if ((i & 255u) == 255u && !report_progress(*this, static_cast<unsigned>(i + 1), static_cast<unsigned>(total))) return false;
    }
    if (!report_progress(*this, static_cast<unsigned>(total), static_cast<unsigned>(total))) return false;
    return commit_plan(*this, history_, plan);
}

namespace {

bool resize_meta(SheetMeta &meta, bool rows, int first, int count, int pixels) {
    Array<int> &values = rows ? meta.row_heights : meta.column_widths;
    if (count <= 0 || first < 0 || first + count > (rows ? MaxRows : MaxCols) ||
        pixels < 0 || pixels == 1 || (pixels > 0 && pixels < 12) || pixels > 600) return false;
    size_t needed = static_cast<size_t>(first + count);
    if (values.size() < needed && !values.resize(needed)) return false;
    for (int i = first; i < first + count; ++i) values[static_cast<size_t>(i)] = pixels;
    while (!values.empty() && values[values.size() - 1] == 0) values.erase(values.size() - 1);
    return true;
}

} // namespace

bool Editor::resize_axis(uint32_t sheet_id, bool rows, int first, int count, int pixels) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || count <= 0 || first < 0 || first + count > (rows ? MaxRows : MaxCols) ||
        pixels < 0 || pixels == 1 || (pixels > 0 && pixels < 12) || pixels > 600) {
        set_error(*this, "行高或列宽无效");
        return false;
    }
    SheetMeta before, after;
    if (!clone_meta(*sheet, before) || !clone_meta(*sheet, after) || !resize_meta(after, rows, first, count, pixels)) {
        set_error(*this, "无法调整行高或列宽");
        return false;
    }
    MutationPlan plan(book, sheet_id);
    return commit_plan(*this, history_, plan, sheet_id, &before, &after);
}

bool Editor::set_view(uint32_t sheet_id, bool freeze_row, bool freeze_column, bool gridlines, int zoom) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || zoom < 25 || zoom > 400) { set_error(*this, "视图设置无效"); return false; }
    SheetMeta before, after;
    if (!clone_meta(*sheet, before) || !clone_meta(*sheet, after)) {
        set_error(*this, "内存不足，操作未执行");
        return false;
    }
    after.freeze_row = freeze_row;
    after.freeze_column = freeze_column;
    after.gridlines = gridlines;
    after.zoom = zoom;
    MutationPlan plan(book, sheet_id);
    return commit_plan(*this, history_, plan, sheet_id, &before, &after);
}

namespace {

const Cell *clipboard_cell(const Clipboard &clipboard, int row, int col) {
    size_t lo = 0, hi = clipboard.cells.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Cell &cell = clipboard.cells[mid];
        if (cell.row < row || (cell.row == row && cell.col < col)) lo = mid + 1;
        else hi = mid;
    }
    return lo < clipboard.cells.size() && clipboard.cells[lo].row == row && clipboard.cells[lo].col == col ?
        &clipboard.cells[lo] : nullptr;
}

bool materialize_paste_cell(Editor &editor, uint32_t source_sheet_id, uint32_t sheet_id, const Cell *source,
                            int source_row, int source_col, int target_row, int target_col,
                            bool values_only, bool translate_relative_refs, CellState &after) {
    after = read_state(editor.book, sheet_id, target_row, target_col);
    if (!source) {
        if (values_only) {
            after.cell.input.clear();
            after.cell.literal = false;
            after.cell.result = Value{};
            after.cell.calculated = 0;
            after.cell.evaluating = 0;
            if (after.cell.style == Style{}) after.present = false;
        } else after.present = false;
        return true;
    }

    Cell copied = *source;
    copied.row = target_row;
    copied.col = target_col;
    copied.result = Value{};
    copied.calculated = 0;
    copied.evaluating = 0;
    if (values_only) {
        Text rendered;
        if (!source->literal && source->input.c_str()[0] == '=') {
            Value value = evaluate(editor.book, source_sheet_id, source_row, source_col);
            rendered = format_value(value, source->style);
        } else rendered = source->input;
        if (rendered.size() > MaxTextBytes) return false;
        if (rendered.empty()) copied.input.clear();
        else copied.input = static_cast<Text &&>(rendered);
        copied.literal = !copied.input.empty();
        const Cell *destination = editor.book.get(sheet_id, target_row, target_col);
        copied.style = destination ? destination->style : Style{};
    } else if (translate_relative_refs && !copied.literal && copied.input.c_str()[0] == '=') {
        Text translated;
        if (!translate_formula(translated, copied.input.c_str(), target_row - source_row, target_col - source_col)) return false;
        copied.input = static_cast<Text &&>(translated);
    }
    after.present = !copied.input.empty() || !(copied.style == Style{});
    after.cell = static_cast<Cell &&>(copied);
    return true;
}

} // namespace

bool Editor::copy_range(uint32_t sheet_id, Range range, Clipboard &output, bool cut) {
    const Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !range.valid()) { set_error(*this, "复制范围无效"); return false; }
    Clipboard next;
    next.sheet = sheet_id;
    next.range = range;
    next.cut = cut;
    next.revision = book.revision;
    size_t count = 0;
    for (size_t i = 0; i < sheet->cells.size(); ++i)
        if (range.contains(sheet->cells[i].row, sheet->cells[i].col)) ++count;
    if (!next.cells.reserve(count)) { set_error(*this, "内存不足，无法复制"); return false; }
    unsigned processed = 0;
    if (!report_progress(*this, 0, static_cast<unsigned>(count))) return false;
    for (size_t i = 0; i < sheet->cells.size(); ++i) {
        if (!range.contains(sheet->cells[i].row, sheet->cells[i].col)) continue;
        if (!next.cells.push(sheet->cells[i])) { set_error(*this, "内存不足，无法复制"); return false; }
        if ((++processed & 255u) == 0 && !report_progress(*this, processed, static_cast<unsigned>(count))) return false;
    }
    if (!report_progress(*this, static_cast<unsigned>(count), static_cast<unsigned>(count))) return false;
    output.sheet = next.sheet;
    output.range = next.range;
    output.cut = next.cut;
    output.revision = next.revision;
    output.cells = static_cast<Array<Cell> &&>(next.cells);
    set_error(*this, nullptr);
    return true;
}

bool Editor::paste(uint32_t sheet_id, int row, int col, Clipboard &clipboard, bool values_only) {
    if (!book.sheet(sheet_id) || !clipboard.range.valid() || row < 0 || col < 0 ||
        row >= MaxRows || col >= MaxCols) { set_error(*this, "粘贴位置无效"); return false; }
    const int height = clipboard.range.r2 - clipboard.range.r1 + 1;
    const int width = clipboard.range.c2 - clipboard.range.c1 + 1;
    if (row + height > MaxRows || col + width > MaxCols) { set_error(*this, "粘贴范围超出工作表"); return false; }
    if (clipboard.cut && (!book.sheet(clipboard.sheet) || clipboard.revision != book.revision)) {
        set_error(*this, "剪切内容已过期");
        return false;
    }
    if (clipboard.cut) {
        if (clipboard.sheet == sheet_id && row == clipboard.range.r1 && col == clipboard.range.c1) {
            clipboard.cut = false;
            set_error(*this, nullptr);
            return true;
        }
        Workbook candidate;
        if (!candidate.clone_from(book)) { set_error(*this, "内存不足，粘贴未执行"); return false; }
        Sheet *source_sheet = candidate.sheet(clipboard.sheet);
        if (!source_sheet || !candidate.sheet(sheet_id)) { set_error(*this, "剪切内容已过期"); return false; }

        bool unsupported = false;
        if (!rewrite_move_references(candidate, clipboard.sheet, clipboard.range, sheet_id,
                                     row, col, unsupported, progress, progress_context)) {
            set_error(*this, "内存不足或操作已取消，剪切未执行");
            return false;
        }
        if (unsupported) {
            set_error(*this, "剪切范围与公式引用范围部分重叠，操作未执行");
            return false;
        }

        // Capture formulas after reference rewriting, then remove the source
        // block in one pass. This avoids quadratic erases for large cuts.
        size_t moved_count = 0;
        for (size_t i = 0; i < source_sheet->cells.size(); ++i)
            if (clipboard.range.contains(source_sheet->cells[i].row, source_sheet->cells[i].col)) ++moved_count;
        Array<Cell> moved_cells, remaining_cells;
        if (!moved_cells.reserve(moved_count) || !remaining_cells.reserve(source_sheet->cells.size())) {
            set_error(*this, "内存不足，粘贴未执行");
            return false;
        }
        const unsigned source_total = static_cast<unsigned>(source_sheet->cells.size());
        if (!report_progress(*this, 0, source_total)) return false;
        for (size_t i = 0; i < source_sheet->cells.size(); ++i) {
            const Cell &cell = source_sheet->cells[i];
            bool ok = clipboard.range.contains(cell.row, cell.col) ?
                moved_cells.push(cell) : remaining_cells.push(cell);
            if (!ok) { set_error(*this, "内存不足，粘贴未执行"); return false; }
            if ((i & 255u) == 255u && !report_progress(*this, static_cast<unsigned>(i + 1), source_total)) return false;
        }
        if (!report_progress(*this, source_total, source_total)) return false;
        source_sheet->cells.swap(remaining_cells);

        Clipboard moved_clipboard;
        moved_clipboard.sheet = clipboard.sheet;
        moved_clipboard.range = clipboard.range;
        moved_clipboard.cells = static_cast<Array<Cell> &&>(moved_cells);
        MutationPlan plan(candidate, sheet_id);
        if (!report_progress(*this, 0, static_cast<unsigned>(height))) return false;
        for (int dr = 0; dr < height; ++dr) {
            for (int dc = 0; dc < width; ++dc) {
                const int source_row = clipboard.range.r1 + dr;
                const int source_col = clipboard.range.c1 + dc;
                const Cell *source = clipboard_cell(moved_clipboard, source_row, source_col);
                CellState after;
                if (!materialize_paste_cell(*this, clipboard.sheet, sheet_id, source, source_row, source_col,
                                            row + dr, col + dc, values_only, false, after)) {
                    set_error(*this, "粘贴内容无效或内存不足，操作未执行");
                    return false;
                }
                CellState before = read_state(candidate, sheet_id, row + dr, col + dc);
                if (!same_cell_value(before, after) && !plan.set(row + dr, col + dc, after)) {
                    set_error(*this, "粘贴内容无效或内存不足，操作未执行");
                    return false;
                }
            }
            if (!report_progress(*this, static_cast<unsigned>(dr + 1), static_cast<unsigned>(height))) return false;
        }
        plan.compact();
        Array<CellState> states;
        if (!states.reserve(plan.size())) { set_error(*this, "内存不足，粘贴未执行"); return false; }
        for (size_t i = 0; i < plan.size(); ++i)
            if (!states.push(plan[i].after)) { set_error(*this, "内存不足，粘贴未执行"); return false; }
        if (!apply_cells(candidate, sheet_id, states)) {
            set_error(*this, "工作簿单元格数量超出限制或内存不足，操作未执行");
            return false;
        }
        if (candidate.cell_count() > MaxCells) {
            set_error(*this, "工作簿单元格数量超出限制，操作未执行");
            return false;
        }
        if (!commit_snapshot(*this, history_, static_cast<Workbook &&>(candidate))) return false;
        clipboard.cut = false;
        return true;
    }
    MutationPlan plan(book, sheet_id);
    if (!report_progress(*this, 0, static_cast<unsigned>(height))) return false;
    for (int dr = 0; dr < height; ++dr) {
        for (int dc = 0; dc < width; ++dc) {
            const int source_row = clipboard.range.r1 + dr;
            const int source_col = clipboard.range.c1 + dc;
            const Cell *source = clipboard_cell(clipboard, source_row, source_col);
            CellState after;
            if (!materialize_paste_cell(*this, clipboard.sheet, sheet_id, source, source_row, source_col,
                                        row + dr, col + dc, values_only, true, after) ||
                !plan.set(row + dr, col + dc, after)) {
                set_error(*this, "粘贴内容无效或内存不足，操作未执行");
                return false;
            }
        }
        if (!report_progress(*this, static_cast<unsigned>(dr + 1), static_cast<unsigned>(height))) return false;
    }
    return commit_plan(*this, history_, plan);
}

bool Editor::fill(uint32_t sheet_id, Range source, Range target) {
    const Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !source.valid() || !target.valid()) { set_error(*this, "填充范围无效"); return false; }
    const int source_height = source.r2 - source.r1 + 1;
    const int source_width = source.c2 - source.c1 + 1;
    MutationPlan plan(book, sheet_id);
    const unsigned total = static_cast<unsigned>(target.r2 - target.r1 + 1);
    if (!report_progress(*this, 0, total)) return false;
    for (int row = target.r1; row <= target.r2; ++row) {
        for (int col = target.c1; col <= target.c2; ++col) {
            int row_offset = (row - source.r1) % source_height;
            int col_offset = (col - source.c1) % source_width;
            if (row_offset < 0) row_offset += source_height;
            if (col_offset < 0) col_offset += source_width;
            int source_row = source.r1 + row_offset;
            int source_col = source.c1 + col_offset;
            const Cell *cell = sheet->get(source_row, source_col);
            CellState after;
            if (!materialize_paste_cell(*this, sheet_id, sheet_id, cell, source_row, source_col,
                                        row, col, false, true, after) || !plan.set(row, col, after)) {
                set_error(*this, "填充内容无效或内存不足，操作未执行");
                return false;
            }
        }
        if (!report_progress(*this, static_cast<unsigned>(row - target.r1 + 1), total)) return false;
    }
    return commit_plan(*this, history_, plan);
}

namespace {

template<class HistoryType>
bool edit_axis(Editor &editor, HistoryType *history, uint32_t sheet_id, bool rows, int first, int count, bool deleted) {
    const int limit = rows ? MaxRows : MaxCols;
    Sheet *original = editor.book.sheet(sheet_id);
    if (!original || count <= 0 || first < 0 || first >= limit || count > limit - first) {
        set_error(editor, "插入或删除范围无效");
        return false;
    }
    const unsigned total = static_cast<unsigned>(original->cells.size());
    if (!report_progress(editor, 0, total)) return false;
    Workbook candidate;
    if (!candidate.clone_from(editor.book)) { set_error(editor, "内存不足，结构操作未执行"); return false; }
    if (!rewrite_axis(candidate, sheet_id, rows, first, count, deleted)) {
        set_error(editor, "无法更新公式引用");
        return false;
    }
    Sheet *sheet = candidate.sheet(sheet_id);
    if (!sheet || !adjust_sheet_for_axis(*sheet, rows, first, count, deleted)) {
        set_error(editor, "此操作会破坏合并单元格或视图范围");
        return false;
    }
    Array<Cell> moved;
    if (!moved.reserve(sheet->cells.size())) { set_error(editor, "内存不足，结构操作未执行"); return false; }
    int last = first + count - 1;
    for (size_t i = 0; i < sheet->cells.size(); ++i) {
        Cell cell = sheet->cells[i];
        int &coordinate = rows ? cell.row : cell.col;
        if (deleted) {
            if (coordinate >= first && coordinate <= last) continue;
            if (coordinate > last) coordinate -= count;
        } else if (coordinate >= first) {
            if (coordinate > limit - 1 - count) {
                set_error(editor, "插入会使现有数据超出工作表范围");
                return false;
            }
            coordinate += count;
        }
        if (!moved.push(static_cast<Cell &&>(cell))) { set_error(editor, "内存不足，结构操作未执行"); return false; }
        if ((i & 255u) == 255u && !report_progress(editor, static_cast<unsigned>(i + 1), total)) return false;
    }
    if (!report_progress(editor, total, total)) return false;
    sheet->cells.swap(moved);
    return commit_snapshot(editor, history, static_cast<Workbook &&>(candidate));
}

} // namespace

bool Editor::insert_axis(uint32_t sheet_id, bool rows, int first, int count) {
    return edit_axis(*this, history_, sheet_id, rows, first, count, false);
}

bool Editor::delete_axis(uint32_t sheet_id, bool rows, int first, int count) {
    return edit_axis(*this, history_, sheet_id, rows, first, count, true);
}

bool Editor::merge(uint32_t sheet_id, Range range, bool remove, bool discard_other) {
    Sheet *original = book.sheet(sheet_id);
    if (!original || !range.valid()) { set_error(*this, "合并范围无效"); return false; }
    bool exact = false;
    for (size_t i = 0; i < original->merges.size(); ++i) {
        if (ranges_equal(original->merges[i], range)) exact = true;
        else if (range_overlaps(original->merges[i], range)) {
            set_error(*this, "合并范围与现有合并单元格重叠");
            return false;
        }
    }
    if (remove) {
        if (!exact) { set_error(*this, "没有匹配的合并范围"); return false; }
    } else {
        if (range.r1 == range.r2 && range.c1 == range.c2) return true;
        if (exact) return true;
        for (size_t i = 0; i < original->cells.size(); ++i) {
            const Cell &cell = original->cells[i];
            if (!range.contains(cell.row, cell.col) || (cell.row == range.r1 && cell.col == range.c1)) continue;
            if (!cell.input.empty() && !discard_other) {
                set_error(*this, "合并会清除其他单元格内容，请确认后重试");
                return false;
            }
        }
    }
    Workbook candidate;
    if (!candidate.clone_from(book)) { set_error(*this, "内存不足，合并未执行"); return false; }
    Sheet *sheet = candidate.sheet(sheet_id);
    if (!sheet) return false;
    if (remove) {
        size_t at = 0;
        while (at < sheet->merges.size() && !ranges_equal(sheet->merges[at], range)) ++at;
        if (at < sheet->merges.size()) sheet->merges.erase(at);
    } else {
        if (!sheet->merges.push(range)) { set_error(*this, "内存不足，合并未执行"); return false; }
        if (discard_other) {
            for (size_t i = 0; i < sheet->cells.size();) {
                Cell &cell = sheet->cells[i];
                if (range.contains(cell.row, cell.col) && !(cell.row == range.r1 && cell.col == range.c1)) {
                    cell.input.clear();
                    cell.literal = false;
                    cell.result = Value{};
                    cell.calculated = 0;
                    cell.evaluating = 0;
                    if (cell.style == Style{}) { sheet->cells.erase(i); continue; }
                }
                ++i;
            }
        }
    }
    return commit_snapshot(*this, history_, static_cast<Workbook &&>(candidate));
}

bool Editor::sort(uint32_t sheet_id, Range range, int column, bool descending, bool header) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !range.valid() || column < range.c1 || column > range.c2) {
        set_error(*this, "排序范围或关键列无效");
        return false;
    }
    int first = range.r1 + (header ? 1 : 0);
    int count = range.r2 - first + 1;
    if (count <= 1) return true;
    for (size_t i = 0; i < sheet->merges.size(); ++i) {
        if (sheet->merges[i].r2 >= first && sheet->merges[i].r1 <= range.r2) {
            set_error(*this, "排序范围内包含合并单元格");
            return false;
        }
    }
    if (!report_progress(*this, 0, static_cast<unsigned>(count))) return false;
    Array<Value> keys;
    Array<int> order, temporary, destination;
    if (!keys.resize(static_cast<size_t>(count)) || !order.resize(static_cast<size_t>(count)) ||
        !temporary.resize(static_cast<size_t>(count)) || !destination.resize(static_cast<size_t>(range.r2 - range.r1 + 1))) {
        set_error(*this, "内存不足，无法排序");
        return false;
    }
    for (int i = 0; i < count; ++i) {
        order[static_cast<size_t>(i)] = i;
        const Cell *cell = sheet->get(first + i, column);
        keys[static_cast<size_t>(i)] = cell ? evaluate(book, sheet_id, first + i, column) : Value{};
        if ((i & 255) == 255 && !report_progress(*this, static_cast<unsigned>(i + 1), static_cast<unsigned>(count))) return false;
    }
    for (int i = 0; i < range.r2 - range.r1 + 1; ++i) destination[static_cast<size_t>(i)] = range.r1 + i;
    for (int width = 1; width < count;) {
        for (int left = 0; left < count; left += width * 2) {
            int middle = left + width;
            if (middle > count) middle = count;
            int right = left + width * 2;
            if (right > count) right = count;
            int a = left, b = middle, out = left;
            while (a < middle && b < right) {
                int comparison = compare_values(keys[static_cast<size_t>(order[static_cast<size_t>(a)])],
                                                keys[static_cast<size_t>(order[static_cast<size_t>(b)])]);
                if (descending) comparison = -comparison;
                if (comparison <= 0) temporary[static_cast<size_t>(out++)] = order[static_cast<size_t>(a++)];
                else temporary[static_cast<size_t>(out++)] = order[static_cast<size_t>(b++)];
            }
            while (a < middle) temporary[static_cast<size_t>(out++)] = order[static_cast<size_t>(a++)];
            while (b < right) temporary[static_cast<size_t>(out++)] = order[static_cast<size_t>(b++)];
        }
        order.swap(temporary);
        if (width > count / 2) break;
        width *= 2;
        if (!report_progress(*this, static_cast<unsigned>(width > count ? count : width), static_cast<unsigned>(count))) return false;
    }
    for (int i = 0; i < count; ++i)
        destination[static_cast<size_t>(first - range.r1 + order[static_cast<size_t>(i)])] = first + i;

    MutationPlan plan(book, sheet_id);
    const size_t total = sheet->cells.size();
    for (size_t i = 0; i < sheet->cells.size(); ++i) {
        const Cell &cell = sheet->cells[i];
        if (cell.row < first || cell.row > range.r2 || cell.col < range.c1 || cell.col > range.c2) continue;
        CellState empty;
        empty.cell.row = cell.row;
        empty.cell.col = cell.col;
        if (!plan.set(cell.row, cell.col, empty)) { set_error(*this, "内存不足，排序未执行"); return false; }
        if ((i & 255u) == 255u && !report_progress(*this, static_cast<unsigned>(i + 1), static_cast<unsigned>(total))) return false;
    }
    for (size_t i = 0; i < sheet->cells.size(); ++i) {
        const Cell &cell = sheet->cells[i];
        if (cell.row < first || cell.row > range.r2 || cell.col < range.c1 || cell.col > range.c2) continue;
        int new_row = destination[static_cast<size_t>(cell.row - range.r1)];
        CellState after;
        after.present = true;
        after.cell = cell;
        after.cell.row = new_row;
        if (!after.cell.literal && after.cell.input.c_str()[0] == '=' && new_row != cell.row) {
            Text translated;
            if (!translate_formula(translated, after.cell.input.c_str(), new_row - cell.row, 0)) {
                set_error(*this, "排序时无法调整公式");
                return false;
            }
            after.cell.input = static_cast<Text &&>(translated);
            after.cell.result = Value{};
            after.cell.calculated = 0;
            after.cell.evaluating = 0;
        }
        if (!plan.set(new_row, cell.col, after)) { set_error(*this, "内存不足，排序未执行"); return false; }
    }
    if (!report_progress(*this, static_cast<unsigned>(total), static_cast<unsigned>(total))) return false;
    return commit_plan(*this, history_, plan);
}

bool Editor::set_filter(uint32_t sheet_id, Range range, int column, const char *query,
                        uint8_t operation, bool header) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !range.valid() || column < range.c1 || column > range.c2 || operation > 4) {
        set_error(*this, "筛选范围或条件无效");
        return false;
    }
    if (!query) query = "";
    size_t query_length = strlen(query);
    if (query_length > MaxTextBytes || !valid_utf8(query, query_length)) {
        set_error(*this, "筛选文本无效或过长");
        return false;
    }
    if ((operation == 2 || operation == 3)) {
        char *end = nullptr;
        double number = strtod(query, &end);
        if (!*query || end == query || *end || !isfinite(number)) {
            set_error(*this, "数值筛选条件无效");
            return false;
        }
    }
    if (!recalculate(book, progress, progress_context)) {
        set_error(*this, "计算已取消，筛选未执行");
        return false;
    }
    SheetMeta before, after;
    if (!clone_meta(*sheet, before) || !clone_meta(*sheet, after)) {
        set_error(*this, "内存不足，筛选未执行");
        return false;
    }
    if (!after.filter_enabled || !ranges_equal(after.filter_range, range)) after.filters.clear();
    after.filter_range = range;
    after.filter_enabled = true;
    after.filter_header = header;
    size_t term_index = 0;
    while (term_index < after.filters.size() && after.filters[term_index].column != column) ++term_index;
    FilterTerm term;
    term.column = column;
    term.operation = operation;
    if (!term.query.set(query, query_length)) { set_error(*this, "内存不足，筛选未执行"); return false; }
    if (term_index < after.filters.size()) after.filters[term_index] = static_cast<FilterTerm &&>(term);
    else if (!after.filters.push(static_cast<FilterTerm &&>(term))) { set_error(*this, "内存不足，筛选未执行"); return false; }
    MutationPlan plan(book, sheet_id);
    return commit_plan(*this, history_, plan, sheet_id, &before, &after);
}

bool Editor::clear_filter(uint32_t sheet_id) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet) { set_error(*this, "工作表已不存在"); return false; }
    if (!sheet->filter_enabled && sheet->filters.empty()) return true;
    SheetMeta before, after;
    if (!clone_meta(*sheet, before) || !clone_meta(*sheet, after)) {
        set_error(*this, "内存不足，筛选未清除");
        return false;
    }
    after.filters.clear();
    after.filter_enabled = false;
    after.filter_header = true;
    after.filter_range = Range{};
    MutationPlan plan(book, sheet_id);
    return commit_plan(*this, history_, plan, sheet_id, &before, &after);
}

bool Editor::replace_all(uint32_t sheet_id, const char *find, const char *replacement,
                         bool match_case, unsigned *count) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet || !find || !replacement || !*find || strlen(find) > MaxTextBytes ||
        strlen(replacement) > MaxTextBytes || !valid_utf8(find, strlen(find)) ||
        !valid_utf8(replacement, strlen(replacement))) {
        set_error(*this, "查找或替换文本无效");
        return false;
    }
    MutationPlan plan(book, sheet_id);
    unsigned replaced_total = 0;
    const size_t total = sheet->cells.size();
    if (!report_progress(*this, 0, static_cast<unsigned>(total))) return false;
    for (size_t i = 0; i < sheet->cells.size(); ++i) {
        const Cell &cell = sheet->cells[i];
        if (cell.input.empty()) continue;
        Text changed;
        unsigned replaced = 0;
        if (!replace_text(cell.input.c_str(), find, replacement, match_case, changed, replaced)) {
            set_error(*this, "替换结果过长或内存不足，操作未执行");
            return false;
        }
        if (!replaced) continue;
        if (replaced > UINT_MAX - replaced_total) { set_error(*this, "替换数量超出限制"); return false; }
        replaced_total += replaced;
        CellState after;
        after.present = true;
        after.cell = cell;
        after.cell.input = static_cast<Text &&>(changed);
        after.cell.result = Value{};
        after.cell.calculated = 0;
        after.cell.evaluating = 0;
        if (!plan.set(cell.row, cell.col, after)) { set_error(*this, "内存不足，替换未执行"); return false; }
        if ((i & 255u) == 255u && !report_progress(*this, static_cast<unsigned>(i + 1), static_cast<unsigned>(total))) return false;
    }
    if (!report_progress(*this, static_cast<unsigned>(total), static_cast<unsigned>(total))) return false;
    if (!commit_plan(*this, history_, plan)) return false;
    if (count) *count = replaced_total;
    return true;
}

bool Editor::add_sheet(const char *name) {
    if (book.sheets.size() >= MaxSheets) { set_error(*this, "工作表数量已达上限"); return false; }
    Workbook candidate;
    if (!candidate.clone_from(book)) { set_error(*this, "内存不足，无法添加工作表"); return false; }
    char generated[32];
    if (!name) {
        unsigned suffix = 1;
        do {
            snprintf(generated, sizeof(generated), "Sheet%u", suffix++);
            if (suffix > 100000) { set_error(*this, "无法生成唯一工作表名称"); return false; }
        } while (candidate.find_sheet(generated));
        name = generated;
    }
    if (!valid_sheet_name(name) || candidate.find_sheet(name)) {
        set_error(*this, "工作表名称无效或重复");
        return false;
    }
    uint32_t id = candidate.next_id;
    if (!id) id = 1;
    uint32_t start = id;
    while (candidate.sheet(id)) {
        if (++id == 0) id = 1;
        if (id == start) { set_error(*this, "没有可用的工作表编号"); return false; }
    }
    Sheet sheet;
    sheet.id = id;
    if (!sheet.name.set(name) || !candidate.sheets.push(static_cast<Sheet &&>(sheet))) {
        set_error(*this, "内存不足，无法添加工作表");
        return false;
    }
    candidate.next_id = id + 1;
    if (!candidate.next_id) candidate.next_id = 1;
    candidate.active_id = id;
    return commit_snapshot(*this, history_, static_cast<Workbook &&>(candidate));
}

bool Editor::rename_sheet(uint32_t sheet_id, const char *name) {
    const Sheet *original = book.sheet(sheet_id);
    if (!original || !valid_sheet_name(name)) { set_error(*this, "工作表名称无效"); return false; }
    uint32_t duplicate = book.find_sheet(name);
    if (duplicate && duplicate != sheet_id) { set_error(*this, "工作表名称已存在"); return false; }
    if (original->name == name) return true;
    Text old_name = original->name;
    Workbook candidate;
    if (!candidate.clone_from(book)) { set_error(*this, "内存不足，无法重命名工作表"); return false; }
    Sheet *sheet = candidate.sheet(sheet_id);
    if (!sheet || !sheet->name.set(name) || !rewrite_sheet_name(candidate, old_name.c_str(), name)) {
        set_error(*this, "无法更新工作表引用");
        return false;
    }
    return commit_snapshot(*this, history_, static_cast<Workbook &&>(candidate));
}

bool Editor::delete_sheet(uint32_t sheet_id) {
    if (book.sheets.size() <= 1) { set_error(*this, "工作簿至少需要一个工作表"); return false; }
    const Sheet *original = book.sheet(sheet_id);
    if (!original) { set_error(*this, "工作表已不存在"); return false; }
    Text old_name = original->name;
    Workbook candidate;
    if (!candidate.clone_from(book) || !rewrite_sheet_name(candidate, old_name.c_str(), nullptr)) {
        set_error(*this, "无法更新工作表引用");
        return false;
    }
    size_t index = 0;
    while (index < candidate.sheets.size() && candidate.sheets[index].id != sheet_id) ++index;
    if (index == candidate.sheets.size()) return false;
    candidate.sheets.erase(index);
    if (candidate.active_id == sheet_id) {
        size_t active = index < candidate.sheets.size() ? index : candidate.sheets.size() - 1;
        candidate.active_id = candidate.sheets[active].id;
    }
    return commit_snapshot(*this, history_, static_cast<Workbook &&>(candidate));
}

bool Editor::move_sheet(uint32_t sheet_id, int direction) {
    if (direction != -1 && direction != 1) { set_error(*this, "工作表移动方向无效"); return false; }
    size_t index = 0;
    while (index < book.sheets.size() && book.sheets[index].id != sheet_id) ++index;
    if (index == book.sheets.size()) { set_error(*this, "工作表已不存在"); return false; }
    if ((direction < 0 && index == 0) || (direction > 0 && index + 1 >= book.sheets.size())) return true;
    Workbook candidate;
    if (!candidate.clone_from(book)) { set_error(*this, "内存不足，无法移动工作表"); return false; }
    size_t destination = direction < 0 ? index - 1 : index + 1;
    Sheet moved = static_cast<Sheet &&>(candidate.sheets[index]);
    if (destination < index) {
        for (size_t i = index; i > destination; --i) candidate.sheets[i] = static_cast<Sheet &&>(candidate.sheets[i - 1]);
    } else {
        for (size_t i = index; i < destination; ++i) candidate.sheets[i] = static_cast<Sheet &&>(candidate.sheets[i + 1]);
    }
    candidate.sheets[destination] = static_cast<Sheet &&>(moved);
    return commit_snapshot(*this, history_, static_cast<Workbook &&>(candidate));
}

namespace {

template<class HistoryType>
bool transfer_history(Editor &editor, HistoryType *history, bool undo_direction) {
    if (!history) return false;
    Array<EditCommand> &source = undo_direction ? history->undo : history->redo;
    Array<EditCommand> &destination = undo_direction ? history->redo : history->undo;
    if (source.empty()) {
        set_error(editor, undo_direction ? "没有可撤销的操作" : "没有可重做的操作");
        return false;
    }
    if (!destination.reserve(destination.size() + 1)) {
        set_error(editor, "内存不足，历史操作未执行");
        return false;
    }
    EditCommand &command = source[source.size() - 1];
    const size_t progress_total = command.kind == CommandKind::Cells ?
        (undo_direction ? command.before_cells.size() : command.after_cells.size()) : editor.book.cell_count();
    if (!report_progress(editor, 0, static_cast<unsigned>(progress_total)) ||
        !report_progress(editor, static_cast<unsigned>(progress_total), static_cast<unsigned>(progress_total))) return false;
    if (command.kind == CommandKind::Snapshot) {
        uint64_t next_revision = editor.book.revision + 1;
        if (!next_revision) next_revision = 1;
        swap_workbooks(editor.book, command.snapshot);
        editor.book.revision = next_revision;
    } else {
        const Array<CellState> &states = undo_direction ? command.before_cells : command.after_cells;
        SheetMeta staged_meta;
        if (command.has_meta) {
            const SheetMeta &meta = undo_direction ? command.before_meta : command.after_meta;
            if (!clone_meta(meta, staged_meta)) {
                set_error(editor, "内存不足，历史操作未执行");
                return false;
            }
        }
        if (command.has_meta && !editor.book.sheet(command.meta_sheet)) {
            set_error(editor, "历史工作表已不存在");
            return false;
        }
        if (!apply_cells(editor.book, command.cell_sheet, states)) {
            set_error(editor, "历史数据超出工作簿限制");
            return false;
        }
        if (command.has_meta) install_meta(*editor.book.sheet(command.meta_sheet), static_cast<SheetMeta &&>(staged_meta));
        editor.book.invalidate();
    }
    if (destination.size() >= HistoryLimit) destination.erase(0);
    destination.push(static_cast<EditCommand &&>(command));
    source.erase(source.size() - 1);
    set_error(editor, nullptr);
    return true;
}

} // namespace

bool Editor::undo() {
    return transfer_history(*this, history_, true);
}

bool Editor::redo() {
    return transfer_history(*this, history_, false);
}

bool Editor::can_undo() const {
    return history_ && !history_->undo.empty();
}

bool Editor::can_redo() const {
    return history_ && !history_->redo.empty();
}

void Editor::reset_history() {
    if (history_) { history_->undo.clear(); history_->redo.clear(); }
}

bool Editor::replace_book(Workbook &&replacement) {
    if (replacement.sheets.empty() || replacement.sheets.size() > MaxSheets ||
        replacement.cell_count() > MaxCells || !replacement.sheet(replacement.active_id)) {
        set_error(*this, "工作簿内容无效");
        return false;
    }
    for (size_t i = 0; i < replacement.sheets.size(); ++i) {
        const Sheet &sheet = replacement.sheets[i];
        if (!sheet.id || !valid_sheet_name(sheet.name.c_str())) { set_error(*this, "工作表名称无效"); return false; }
        for (size_t j = i + 1; j < replacement.sheets.size(); ++j) {
            if (replacement.sheets[j].id == sheet.id ||
                compare_text(replacement.sheets[j].name.c_str(), sheet.name.c_str()) == 0) {
                set_error(*this, "工作表编号或名称重复");
                return false;
            }
        }
        for (size_t c = 0; c < sheet.cells.size(); ++c) {
            const Cell &cell = sheet.cells[c];
            if (cell.row < 0 || cell.row >= MaxRows || cell.col < 0 || cell.col >= MaxCols ||
                cell.input.size() > MaxTextBytes || !valid_utf8(cell.input.c_str(), cell.input.size()) ||
                (c && (sheet.cells[c - 1].row > cell.row ||
                       (sheet.cells[c - 1].row == cell.row && sheet.cells[c - 1].col >= cell.col)))) {
                set_error(*this, "单元格数据无效");
                return false;
            }
        }
    }
    uint64_t next_revision = book.revision + 1;
    if (!next_revision) next_revision = 1;
    book = static_cast<Workbook &&>(replacement);
    book.revision = next_revision;
    reset_history();
    set_error(*this, nullptr);
    return true;
}

} // namespace rx
