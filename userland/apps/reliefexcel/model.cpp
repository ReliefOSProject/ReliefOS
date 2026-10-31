#include "model.h"
#include "formula.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rx {
namespace {

bool style_is_default(const Style &style) {
    return style == Style{};
}

int compare_ascii_case(const char *left, const char *right) {
    if (!left || !right) return left ? 1 : (right ? -1 : 0);
    while (*left && *right) {
        unsigned char a = static_cast<unsigned char>(*left++);
        unsigned char b = static_cast<unsigned char>(*right++);
        if (a < 0x80) a = static_cast<unsigned char>(tolower(a));
        if (b < 0x80) b = static_cast<unsigned char>(tolower(b));
        if (a != b) return a < b ? -1 : 1;
    }
    if (*left) return 1;
    if (*right) return -1;
    return 0;
}

int cell_position(const Array<Cell> &cells, int row, int col, bool &found) {
    size_t lo = 0, hi = cells.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Cell &cell = cells[mid];
        if (cell.row < row || (cell.row == row && cell.col < col)) lo = mid + 1;
        else hi = mid;
    }
    found = lo < cells.size() && cells[lo].row == row && cells[lo].col == col;
    return static_cast<int>(lo);
}

bool parse_number(const char *text, double &number) {
    if (!text || !*text) return false;
    errno = 0;
    char *end = nullptr;
    double value = strtod(text, &end);
    if (end == text || *end || errno == ERANGE || !isfinite(value)) return false;
    number = value;
    return true;
}

bool value_text(const Cell *cell, Text &output) {
    if (!cell) return true;
    if (cell->result.kind != ValueKind::Empty) {
        output = format_value(cell->result);
        return true;
    }
    return output.set(cell->input.c_str(), cell->input.size());
}

bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    const size_t n = strlen(needle);
    if (!n) return true;
    for (const char *at = haystack; *at; ++at) {
        size_t i = 0;
        while (i < n && at[i]) {
            unsigned char a = static_cast<unsigned char>(at[i]);
            unsigned char b = static_cast<unsigned char>(needle[i]);
            if (a < 0x80) a = static_cast<unsigned char>(tolower(a));
            if (b < 0x80) b = static_cast<unsigned char>(tolower(b));
            if (a != b) break;
            ++i;
        }
        if (i == n) return true;
    }
    return false;
}

} // namespace

bool Style::operator==(const Style &other) const {
    return foreground == other.foreground && background == other.background &&
        size == other.size && bold == other.bold && italic == other.italic &&
        wrap == other.wrap && borders == other.borders && alignment == other.alignment &&
        format == other.format;
}

Value Value::numeric(double value) {
    Value result;
    if (!isfinite(value)) return failure("#NUM!");
    result.kind = ValueKind::Number;
    result.number = value;
    return result;
}

Value Value::boolean(bool value) {
    Value result;
    result.kind = ValueKind::Boolean;
    if (!result.text.set(value ? "TRUE" : "FALSE")) result.kind = ValueKind::Empty;
    return result;
}

Value Value::string(const char *text) {
    Value result;
    if (text && result.text.set(text)) result.kind = ValueKind::Text;
    return result;
}

Value Value::failure(const char *error) {
    Value result;
    if (error && result.text.set(error)) result.kind = ValueKind::Error;
    return result;
}

Text format_value(const Value &value, const Style &style) {
    Text output;
    char buffer[128];
    const char *text = nullptr;
    switch (value.kind) {
    case ValueKind::Empty:
        return output;
    case ValueKind::Text:
    case ValueKind::Boolean:
    case ValueKind::Error:
        output = value.text;
        return output;
    case ValueKind::Number:
        switch (style.format) {
        case NumberFormat::Number:
            snprintf(buffer, sizeof(buffer), "%.2f", value.number);
            break;
        case NumberFormat::Percent:
            snprintf(buffer, sizeof(buffer), "%.2f%%", value.number * 100.0);
            break;
        case NumberFormat::Currency:
            snprintf(buffer, sizeof(buffer), "¥%.2f", value.number);
            break;
        case NumberFormat::General:
        default:
            snprintf(buffer, sizeof(buffer), "%.15g", value.number);
            break;
        }
        text = buffer;
        break;
    }
    if (text) output.set(text, strlen(text));
    return output;
}

bool valid_utf8(const char *text, size_t length) {
    if (!text && length) return false;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(text);
    size_t i = 0;
    while (i < length) {
        unsigned char a = bytes[i++];
        if (a <= 0x7f) continue;
        if (a >= 0xc2 && a <= 0xdf) {
            if (i >= length || (bytes[i] & 0xc0) != 0x80) return false;
            ++i;
            continue;
        }
        if (a >= 0xe0 && a <= 0xef) {
            if (length - i < 2) return false;
            unsigned char b = bytes[i], c = bytes[i + 1];
            if ((b & 0xc0) != 0x80 || (c & 0xc0) != 0x80) return false;
            if ((a == 0xe0 && b < 0xa0) || (a == 0xed && b >= 0xa0)) return false;
            i += 2;
            continue;
        }
        if (a >= 0xf0 && a <= 0xf4) {
            if (length - i < 3) return false;
            unsigned char b = bytes[i], c = bytes[i + 1], d = bytes[i + 2];
            if ((b & 0xc0) != 0x80 || (c & 0xc0) != 0x80 || (d & 0xc0) != 0x80) return false;
            if ((a == 0xf0 && b < 0x90) || (a == 0xf4 && b >= 0x90)) return false;
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

bool valid_sheet_name(const char *name) {
    if (!name || !*name || !valid_utf8(name, strlen(name))) return false;
    const size_t bytes = strlen(name);
    if (name[0] == '\'' || name[bytes - 1] == '\'') return false;
    size_t units = 0;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(name);
    size_t at = 0;
    while (at < bytes) {
        unsigned char c = p[at];
        if (c < 0x20 || c == 0x7f || c == '[' || c == ']' || c == ':' || c == '*' ||
            c == '?' || c == '/' || c == '\\') return false;
        if (c < 0x80) { ++units; ++at; }
        else if (c < 0xe0) { ++units; at += 2; }
        else if (c < 0xf0) { ++units; at += 3; }
        else { units += 2; at += 4; }
        if (units > 31) return false;
    }
    return true;
}

bool column_label(int col, char *buffer, size_t size) {
    if (col < 0 || col >= MaxCols || !buffer || !size) return false;
    char reverse[8];
    size_t length = 0;
    unsigned value = static_cast<unsigned>(col) + 1;
    while (value && length < sizeof(reverse)) {
        --value;
        reverse[length++] = static_cast<char>('A' + value % 26);
        value /= 26;
    }
    if (length + 1 > size) return false;
    for (size_t i = 0; i < length; ++i) buffer[i] = reverse[length - i - 1];
    buffer[length] = 0;
    return true;
}

bool parse_address(const char *text, int &row, int &col) {
    if (!text || !*text) return false;
    const char *p = text;
    if (*p == '$') ++p;
    unsigned column = 0;
    unsigned letters = 0;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        unsigned char letter = static_cast<unsigned char>(toupper(static_cast<unsigned char>(*p++)));
        if (++letters > 3) return false;
        column = column * 26 + static_cast<unsigned>(letter - 'A' + 1);
        if (column > static_cast<unsigned>(MaxCols)) return false;
    }
    if (!letters) return false;
    if (*p == '$') ++p;
    unsigned number = 0;
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9') {
        number = number * 10 + static_cast<unsigned>(*p++ - '0');
        if (++digits > 5 || number > static_cast<unsigned>(MaxRows)) return false;
    }
    if (!digits || *p || number == 0) return false;
    row = static_cast<int>(number - 1);
    col = static_cast<int>(column - 1);
    return true;
}

bool Sheet::clone_from(const Sheet &other) {
    Array<Cell> new_cells;
    Array<Range> new_merges;
    Array<int> new_widths, new_heights;
    Array<FilterTerm> new_filters;
    if (!new_cells.clone_from(other.cells) || !new_merges.clone_from(other.merges) ||
        !new_widths.clone_from(other.column_widths) || !new_heights.clone_from(other.row_heights) ||
        !new_filters.clone_from(other.filters)) return false;

    id = other.id;
    name = other.name;
    cells.swap(new_cells);
    merges.swap(new_merges);
    column_widths.swap(new_widths);
    row_heights.swap(new_heights);
    filters.swap(new_filters);
    filter_range = other.filter_range;
    filter_enabled = other.filter_enabled;
    filter_header = other.filter_header;
    freeze_row = other.freeze_row;
    freeze_column = other.freeze_column;
    gridlines = other.gridlines;
    zoom = other.zoom;
    cursor_row = other.cursor_row;
    cursor_col = other.cursor_col;
    scroll_row = other.scroll_row;
    scroll_col = other.scroll_col;
    return true;
}

const Cell *Sheet::get(int row, int col) const {
    if (row < 0 || row >= MaxRows || col < 0 || col >= MaxCols) return nullptr;
    bool found = false;
    int at = cell_position(cells, row, col, found);
    return found ? &cells[static_cast<size_t>(at)] : nullptr;
}

Cell *Sheet::get(int row, int col) {
    return const_cast<Cell *>(static_cast<const Sheet *>(this)->get(row, col));
}

bool Sheet::set_input(int row, int col, const char *input, bool literal) {
    if (row < 0 || row >= MaxRows || col < 0 || col >= MaxCols || !input) return false;
    size_t length = strlen(input);
    if (length > MaxTextBytes || !valid_utf8(input, length)) return false;
    bool found = false;
    int at = cell_position(cells, row, col, found);
    if (!length) {
        if (!found) return true;
        Cell &current = cells[static_cast<size_t>(at)];
        current.input.clear();
        current.literal = false;
        current.result = Value{};
        current.calculated = 0;
        current.evaluating = 0;
        if (style_is_default(current.style)) cells.erase(static_cast<size_t>(at));
        return true;
    }

    Text content;
    if (!content.set(input, length)) return false;
    if (!found) {
        if (cells.size() >= MaxCells) return false;
        Cell cell;
        cell.row = row;
        cell.col = col;
        cell.input = static_cast<Text &&>(content);
        cell.literal = literal;
        if (!cells.insert(static_cast<size_t>(at), cell)) return false;
    } else {
        Cell &cell = cells[static_cast<size_t>(at)];
        cell.input = static_cast<Text &&>(content);
        cell.literal = literal;
        cell.result = Value{};
        cell.calculated = 0;
        cell.evaluating = 0;
    }
    return true;
}

bool Sheet::set_style(int row, int col, const Style &style) {
    if (row < 0 || row >= MaxRows || col < 0 || col >= MaxCols) return false;
    bool found = false;
    int at = cell_position(cells, row, col, found);
    if (!found) {
        if (style_is_default(style)) return true;
        if (cells.size() >= MaxCells) return false;
        Cell cell;
        cell.row = row;
        cell.col = col;
        cell.style = style;
        return cells.insert(static_cast<size_t>(at), cell);
    }
    Cell &cell = cells[static_cast<size_t>(at)];
    cell.style = style;
    if (cell.input.empty() && style_is_default(cell.style)) cells.erase(static_cast<size_t>(at));
    return true;
}

bool Sheet::row_visible(int row) const {
    if (!filter_enabled || row < 0 || !filter_range.valid() || !filter_range.contains(row, filter_range.c1)) return true;
    if (filter_header && row == filter_range.r1) return true;
    for (size_t i = 0; i < filters.size(); ++i) {
        const FilterTerm &term = filters[i];
        const Cell *cell = get(row, term.column);
        if (term.operation == 4) {
            if (!cell || cell->input.empty()) return false;
            continue;
        }
        if (!cell || cell->input.empty()) return false;
        if (term.operation == 0 || term.operation == 1) {
            Text displayed;
            if (!value_text(cell, displayed)) return false;
            bool matches = term.operation == 0 ?
                contains_case_insensitive(displayed.c_str(), term.query.c_str()) :
                compare_ascii_case(displayed.c_str(), term.query.c_str()) == 0;
            if (!matches) return false;
        } else if (term.operation == 2 || term.operation == 3) {
            double wanted = 0, actual = 0;
            if (!parse_number(term.query.c_str(), wanted)) return false;
            if (cell->result.kind == ValueKind::Number) actual = cell->result.number;
            else if (!parse_number(cell->input.c_str(), actual)) return false;
            if (term.operation == 2 ? actual <= wanted : actual >= wanted) return false;
        } else {
            return false;
        }
    }
    return true;
}

int Sheet::used_rows() const {
    int used = 0;
    for (size_t i = 0; i < cells.size(); ++i) if (cells[i].row + 1 > used) used = cells[i].row + 1;
    for (size_t i = 0; i < merges.size(); ++i) if (merges[i].r2 + 1 > used) used = merges[i].r2 + 1;
    return used;
}

int Sheet::used_columns() const {
    int used = 0;
    for (size_t i = 0; i < cells.size(); ++i) if (cells[i].col + 1 > used) used = cells[i].col + 1;
    for (size_t i = 0; i < merges.size(); ++i) if (merges[i].c2 + 1 > used) used = merges[i].c2 + 1;
    return used;
}

bool Workbook::init() {
    Workbook next;
    Sheet sheet;
    sheet.id = 1;
    if (!sheet.name.set("Sheet1") || !next.sheets.push(static_cast<Sheet &&>(sheet))) return false;
    next.next_id = 2;
    next.active_id = 1;
    next.revision = revision ? revision + 1 : 1;
    if (!next.revision) next.revision = 1;
    sheets.swap(next.sheets);
    next_id = next.next_id;
    active_id = next.active_id;
    revision = next.revision;
    return true;
}

bool Workbook::clone_from(const Workbook &other) {
    if (other.sheets.size() > MaxSheets) return false;
    Workbook next;
    if (!next.sheets.reserve(other.sheets.size())) return false;
    for (size_t i = 0; i < other.sheets.size(); ++i) {
        Sheet copy;
        if (!copy.clone_from(other.sheets[i]) || !next.sheets.push(static_cast<Sheet &&>(copy))) return false;
    }
    if (other.active_id && !next.sheet(other.active_id)) return false;
    next.next_id = other.next_id;
    next.active_id = other.active_id;
    next.revision = other.revision;
    sheets.swap(next.sheets);
    next_id = next.next_id;
    active_id = next.active_id;
    revision = next.revision;
    return true;
}

Sheet *Workbook::sheet(uint32_t id) {
    for (size_t i = 0; i < sheets.size(); ++i) if (sheets[i].id == id) return &sheets[i];
    return nullptr;
}

const Sheet *Workbook::sheet(uint32_t id) const {
    for (size_t i = 0; i < sheets.size(); ++i) if (sheets[i].id == id) return &sheets[i];
    return nullptr;
}

const Cell *Workbook::get(uint32_t id, int row, int col) const {
    const Sheet *target = sheet(id);
    return target ? target->get(row, col) : nullptr;
}

Cell *Workbook::get(uint32_t id, int row, int col) {
    Sheet *target = sheet(id);
    return target ? target->get(row, col) : nullptr;
}

bool Workbook::set_input(uint32_t id, int row, int col, const char *input, bool literal) {
    Sheet *target = sheet(id);
    if (!target || !input || row < 0 || row >= MaxRows || col < 0 || col >= MaxCols) return false;
    size_t length = strlen(input);
    if (length > MaxTextBytes || !valid_utf8(input, length)) return false;
    bool adding = length && !target->get(row, col);
    if (adding && cell_count() >= MaxCells) return false;
    if (!target->set_input(row, col, input, literal)) return false;
    invalidate();
    return true;
}

uint32_t Workbook::find_sheet(const char *name) const {
    if (!name) return 0;
    for (size_t i = 0; i < sheets.size(); ++i)
        if (compare_ascii_case(sheets[i].name.c_str(), name) == 0) return sheets[i].id;
    return 0;
}

size_t Workbook::cell_count() const {
    size_t count = 0;
    for (size_t i = 0; i < sheets.size(); ++i) {
        if (sheets[i].cells.size() > SIZE_MAX - count) return SIZE_MAX;
        count += sheets[i].cells.size();
    }
    return count;
}

void Workbook::invalidate() {
    if (++revision == 0) revision = 1;
}

} // namespace rx
