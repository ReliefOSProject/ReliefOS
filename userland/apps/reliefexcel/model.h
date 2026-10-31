#ifndef RELIEFEXCEL_MODEL_H
#define RELIEFEXCEL_MODEL_H
#include "storage.h"
namespace rx {
constexpr int MaxRows = 10000, MaxCols = 256, MaxSheets = 16;
constexpr size_t MaxCells = 500000, MaxTextBytes = 32768;
using Progress = bool (*)(void *, unsigned, unsigned);
struct Range {
    int r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    bool valid() const { return r1 >= 0 && c1 >= 0 && r2 >= r1 && c2 >= c1 && r2 < MaxRows && c2 < MaxCols; }
    bool contains(int row, int col) const { return row >= r1 && row <= r2 && col >= c1 && col <= c2; }
};
enum class NumberFormat : uint8_t { General, Number, Percent, Currency };
enum class Alignment : uint8_t { Auto, Left, Center, Right };
struct Style {
    uint32_t foreground = 0x202520, background = 0xffffff;
    uint16_t size = 14;
    uint8_t bold = 0, italic = 0, wrap = 0, borders = 0;
    Alignment alignment = Alignment::Auto;
    NumberFormat format = NumberFormat::General;
    bool operator==(const Style &other) const;
};
enum class ValueKind : uint8_t { Empty, Number, Text, Boolean, Error };
struct Value {
    ValueKind kind = ValueKind::Empty;
    double number = 0;
    Text text;
    static Value numeric(double number);
    static Value boolean(bool value);
    static Value string(const char *text);
    static Value failure(const char *error);
};
struct Cell {
    int row = 0, col = 0;
    Text input;
    Style style;
    bool literal = false;
    Value result;
    uint64_t calculated = 0;
    uint8_t evaluating = 0;
};
struct FilterTerm { int column = 0; Text query; uint8_t operation = 0; }; // contains, equal, >, <, nonempty
struct Sheet {
    uint32_t id = 0;
    Text name;
    Array<Cell> cells; // sorted row-major; only nonempty or styled cells
    Array<Range> merges;
    Array<int> column_widths, row_heights;
    Array<FilterTerm> filters;
    Range filter_range;
    bool filter_enabled = false, filter_header = true;
    bool freeze_row = false, freeze_column = false, gridlines = true;
    int zoom = 100, cursor_row = 0, cursor_col = 0, scroll_row = 0, scroll_col = 0;
    Sheet() = default;
    Sheet(Sheet &&) noexcept = default;
    Sheet &operator=(Sheet &&) noexcept = default;
    bool clone_from(const Sheet &other);
    const Cell *get(int row, int col) const;
    Cell *get(int row, int col);
    bool set_input(int row, int col, const char *input, bool literal = false);
    bool set_style(int row, int col, const Style &style);
    bool row_visible(int row) const;
    int used_rows() const;
    int used_columns() const;
};
struct Workbook {
    Array<Sheet> sheets;
    uint32_t next_id = 1, active_id = 0;
    uint64_t revision = 1;
    bool init();
    bool clone_from(const Workbook &other);
    Sheet *sheet(uint32_t id);
    const Sheet *sheet(uint32_t id) const;
    const Cell *get(uint32_t id, int row, int col) const;
    Cell *get(uint32_t id, int row, int col);
    bool set_input(uint32_t id, int row, int col, const char *input, bool literal = false);
    uint32_t find_sheet(const char *name) const;
    size_t cell_count() const;
    void invalidate();
};
bool valid_utf8(const char *text, size_t length);
bool valid_sheet_name(const char *name);
Text format_value(const Value &value, const Style &style = Style{});
bool column_label(int col, char *buffer, size_t size);
bool parse_address(const char *text, int &row, int &col);

struct Clipboard { uint32_t sheet = 0; Range range; Array<Cell> cells; bool cut = false; uint64_t revision = 0; };
class Editor {
    struct History;
    History *history_ = nullptr;
public:
    Workbook book;
    Text error;
    Progress progress = nullptr;
    void *progress_context = nullptr;
    Editor();
    ~Editor();
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;
    bool set_cell(uint32_t sheet, int row, int col, const char *input, bool literal = false);
    bool style_range(uint32_t sheet, Range range, const Style &style);
    bool patch_style(uint32_t sheet, Range range, const Style &style, uint32_t mask);
    bool clear_range(uint32_t sheet, Range range);
    bool resize_axis(uint32_t sheet, bool rows, int first, int count, int pixels);
    bool set_view(uint32_t sheet, bool freeze_row, bool freeze_column, bool gridlines, int zoom);
    bool copy_range(uint32_t sheet, Range range, Clipboard &output, bool cut = false);
    bool paste(uint32_t sheet, int row, int col, Clipboard &clipboard, bool values_only = false);
    bool fill(uint32_t sheet, Range source, Range target);
    bool insert_axis(uint32_t sheet, bool rows, int first, int count);
    bool delete_axis(uint32_t sheet, bool rows, int first, int count);
    bool merge(uint32_t sheet, Range range, bool remove = false, bool discard_other = false);
    bool sort(uint32_t sheet, Range range, int column, bool descending, bool header);
    bool set_filter(uint32_t sheet, Range range, int column, const char *query, uint8_t operation, bool header = true);
    bool clear_filter(uint32_t sheet);
    bool replace_all(uint32_t sheet, const char *find, const char *replacement, bool match_case, unsigned *count = nullptr);
    bool add_sheet(const char *name = nullptr);
    bool rename_sheet(uint32_t sheet, const char *name);
    bool delete_sheet(uint32_t sheet);
    bool move_sheet(uint32_t sheet, int direction);
    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;
    void reset_history();
    bool replace_book(Workbook &&replacement); // successful open/import; clears history
};
}
#endif
