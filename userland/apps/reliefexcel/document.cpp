#include "document.h"
#include "formula.h"
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <libintl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rx {
namespace {

constexpr uint16_t NativeVersion = 1;
constexpr uint64_t MaxDocumentBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t MaxDelimitedBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t MaxAggregateTextBytes = 128ULL * 1024ULL * 1024ULL;
constexpr size_t IoProgressQuantum = 64 * 1024;
constexpr size_t WriteBufferSize = 32 * 1024;

bool set_error(Text &error, const char *message) {
    if (!error.set(gettext(message))) error.clear();
    return false;
}

bool set_errno_error(Text &error, const char *operation) {
    int saved_errno = errno;
    char detail[128];
    const char *system_detail = strerror(saved_errno);
    int detail_length = snprintf(detail, sizeof(detail), "%s",
                                 system_detail ? system_detail : "I/O error");
    if (detail_length < 0 || (size_t)detail_length >= sizeof(detail))
        memcpy(detail, "I/O error", sizeof("I/O error"));

    const char *translated_operation = gettext(operation);
    char message[256];
    int n = snprintf(message, sizeof(message), "%s: %s", translated_operation, detail);
    if (n < 0 || (size_t)n >= sizeof(message)) return set_error(error, translated_operation);
    return set_error(error, message);
}

bool valid_bytes(const char *bytes, size_t length) {
    if ((!bytes && length) || length > MaxTextBytes || !valid_utf8(bytes ? bytes : "", length))
        return false;
    for (size_t i = 0; i < length; ++i) if (!bytes[i]) return false;
    return true;
}

bool checked_add(uint64_t &value, uint64_t amount) {
    if (amount > UINT64_MAX - value) return false;
    value += amount;
    return value <= MaxDocumentBytes;
}

bool progress_call(Progress progress, void *context, unsigned completed, unsigned total) {
    return !progress || progress(context, completed, total);
}

uint32_t load_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t load_u64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}

void store_u16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

void store_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

void store_u64(unsigned char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (i * 8));
}

bool validate_sheet(const Sheet &sheet, uint64_t &cell_total, uint64_t &text_total,
                    Text &error) {
    if (!sheet.id || !sheet.name.size() || !valid_bytes(sheet.name.c_str(), sheet.name.size()) ||
        !valid_sheet_name(sheet.name.c_str()))
        return set_error(error, "Invalid worksheet name or identifier");
    if (sheet.zoom < 25 || sheet.zoom > 400 || sheet.cursor_row < 0 ||
        sheet.cursor_row >= MaxRows || sheet.cursor_col < 0 || sheet.cursor_col >= MaxCols ||
        sheet.scroll_row < 0 || sheet.scroll_row >= MaxRows ||
        sheet.scroll_col < 0 || sheet.scroll_col >= MaxCols)
        return set_error(error, "Invalid worksheet view settings");
    if (sheet.column_widths.size() > (size_t)MaxCols ||
        sheet.row_heights.size() > (size_t)MaxRows)
        return set_error(error, "Worksheet dimensions exceed resource limits");
    for (size_t i = 0; i < sheet.column_widths.size(); ++i)
        if (sheet.column_widths[i] < 0 || sheet.column_widths[i] > 4096)
            return set_error(error, "Invalid column width");
    for (size_t i = 0; i < sheet.row_heights.size(); ++i)
        if (sheet.row_heights[i] < 0 || sheet.row_heights[i] > 4096)
            return set_error(error, "Invalid row height");
    if (!sheet.filter_range.valid()) return set_error(error, "Invalid filter range");
    if (sheet.filters.size() > (size_t)MaxCols)
        return set_error(error, "Too many worksheet filters");
    for (size_t i = 0; i < sheet.filters.size(); ++i) {
        const FilterTerm &filter = sheet.filters[i];
        if (filter.column < 0 || filter.column >= MaxCols || filter.operation > 4 ||
            !valid_bytes(filter.query.c_str(), filter.query.size()))
            return set_error(error, "Invalid worksheet filter");
        if (!checked_add(text_total, filter.query.size()))
            return set_error(error, "Workbook text exceeds the resource limit");
    }
    if (sheet.merges.size() > MaxCells)
        return set_error(error, "Too many merged ranges");
    unsigned char *merge_occupancy = nullptr;
    if (sheet.merges.size()) {
        merge_occupancy = (unsigned char *)calloc((size_t)MaxRows * (size_t)MaxCols, 1);
        if (!merge_occupancy) return set_error(error, "Not enough memory to validate merged ranges");
    }
    for (size_t i = 0; i < sheet.merges.size(); ++i) {
        const Range &range = sheet.merges[i];
        if (!range.valid() || (range.r1 == range.r2 && range.c1 == range.c2)) {
            free(merge_occupancy);
            return set_error(error, "Invalid merged range");
        }
        bool overlaps = false;
        for (int row = range.r1; row <= range.r2 && !overlaps; ++row) {
            size_t at = (size_t)row * (size_t)MaxCols + (size_t)range.c1;
            for (int column = range.c1; column <= range.c2; ++column, ++at) {
                if (merge_occupancy[at]) {
                    overlaps = true;
                    break;
                }
                merge_occupancy[at] = 1;
            }
        }
        if (overlaps) {
            free(merge_occupancy);
                return set_error(error, "Overlapping merged ranges");
        }
    }
    free(merge_occupancy);
    if (sheet.cells.size() > MaxCells - cell_total)
        return set_error(error, "Workbook exceeds the cell limit");
    int previous = -1;
    for (size_t i = 0; i < sheet.cells.size(); ++i) {
        const Cell &cell = sheet.cells[i];
        if (cell.row < 0 || cell.row >= MaxRows || cell.col < 0 || cell.col >= MaxCols)
            return set_error(error, "Cell coordinate is outside worksheet bounds");
        const int key = cell.row * MaxCols + cell.col;
        if (key <= previous) return set_error(error, "Duplicate or unordered cell coordinate");
        previous = key;
        if (!valid_bytes(cell.input.c_str(), cell.input.size()))
            return set_error(error, "Invalid cell text");
        if (cell.style.size < 6 || cell.style.size > 96 ||
            cell.style.bold > 1 || cell.style.italic > 1 || cell.style.wrap > 1 ||
            cell.style.borders > 15 || (unsigned)cell.style.alignment > (unsigned)Alignment::Right ||
            (unsigned)cell.style.format > (unsigned)NumberFormat::Currency)
            return set_error(error, "Invalid cell style");
        if (!checked_add(text_total, cell.input.size()))
            return set_error(error, "Workbook text exceeds the resource limit");
    }
    cell_total += sheet.cells.size();
    return true;
}

bool validate_workbook(const Workbook &book, uint64_t &file_size, Text &error) {
    if (book.sheets.empty() || book.sheets.size() > (size_t)MaxSheets ||
        !book.next_id || !book.active_id || !book.revision)
        return set_error(error, "Invalid workbook metadata");
    bool active_found = false;
    uint32_t max_id = 0;
    uint64_t cells = 0, text = 0;
    uint64_t size = 16 + 20;
    for (size_t i = 0; i < book.sheets.size(); ++i) {
        const Sheet &sheet = book.sheets[i];
        if (sheet.id == book.active_id) active_found = true;
        if (sheet.id > max_id) max_id = sheet.id;
        for (size_t j = 0; j < i; ++j) {
            if (book.sheets[j].id == sheet.id)
                return set_error(error, "Duplicate worksheet identifier");
            if (!strcasecmp(book.sheets[j].name.c_str(), sheet.name.c_str()))
                return set_error(error, "Duplicate worksheet name");
        }
        if (!validate_sheet(sheet, cells, text, error)) return false;
        if (!checked_add(size, 4 + 4 + sheet.name.size() + 20 + 5 + 4 + sheet.column_widths.size() * 4ULL +
            4 + sheet.row_heights.size() * 4ULL + 16 + 4 + 4 + sheet.merges.size() * 16ULL + 4))
            return set_error(error, "Workbook exceeds the file size limit");
        for (size_t j = 0; j < sheet.filters.size(); ++j)
            if (!checked_add(size, 4 + 1 + 4 + sheet.filters[j].query.size()))
                return set_error(error, "Workbook exceeds the file size limit");
        for (size_t j = 0; j < sheet.cells.size(); ++j)
            if (!checked_add(size, 29 + sheet.cells[j].input.size()))
                return set_error(error, "Workbook exceeds the file size limit");
    }
    if (!active_found || book.next_id <= max_id)
        return set_error(error, "Invalid active worksheet or identifier sequence");
    if (text > MaxAggregateTextBytes) return set_error(error, "Workbook text exceeds the resource limit");
    file_size = size;
    return true;
}

struct FileWriter {
    int fd = -1;
    uint64_t written = 0, total = 0, next_progress = 0;
    unsigned char buffer[WriteBufferSize];
    size_t buffered = 0;
    Progress progress = nullptr;
    void *context = nullptr;
    bool failed = false;
    Text *error = nullptr;

    bool flush() {
        const unsigned char *at = buffer;
        size_t left = buffered;
        while (left) {
            ssize_t count = ::write(fd, at, left);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                failed = true;
                if (error) set_errno_error(*error, "Could not write workbook");
                return false;
            }
            at += count;
            left -= (size_t)count;
            written += (uint64_t)count;
            if (progress && (written >= next_progress || written == total)) {
                if (!progress_call(progress, context,
                        (unsigned)(written > UINT_MAX ? UINT_MAX : written),
                        (unsigned)(total > UINT_MAX ? UINT_MAX : total))) {
                    failed = true;
                    if (error) set_error(*error, "Operation cancelled");
                    return false;
                }
                next_progress = written + IoProgressQuantum;
            }
        }
        buffered = 0;
        return true;
    }
    bool write(const void *data, size_t size) {
        const unsigned char *at = (const unsigned char *)data;
        size_t left = size;
        while (left) {
            size_t available = sizeof(buffer) - buffered;
            size_t count = left < available ? left : available;
            memcpy(buffer + buffered, at, count);
            buffered += count;
            at += count;
            left -= count;
            if (buffered == sizeof(buffer) && !flush()) return false;
        }
        return true;
    }
    bool u8(uint8_t value) { return write(&value, 1); }
    bool u16(uint16_t value) {
        unsigned char bytes[2]; store_u16(bytes, value); return write(bytes, sizeof(bytes));
    }
    bool u32(uint32_t value) {
        unsigned char bytes[4]; store_u32(bytes, value); return write(bytes, sizeof(bytes));
    }
    bool u64(uint64_t value) {
        unsigned char bytes[8]; store_u64(bytes, value); return write(bytes, sizeof(bytes));
    }
    bool i32(int value) { return u32((uint32_t)(int32_t)value); }
    bool string(const Text &value) {
        return value.size() <= UINT32_MAX && u32((uint32_t)value.size()) &&
            write(value.c_str(), value.size());
    }
};

bool write_range(FileWriter &writer, const Range &range) {
    return writer.i32(range.r1) && writer.i32(range.c1) &&
        writer.i32(range.r2) && writer.i32(range.c2);
}

bool write_sheet(FileWriter &writer, const Sheet &sheet) {
    if (!writer.u32(sheet.id) || !writer.string(sheet.name) || !writer.i32(sheet.zoom) ||
        !writer.i32(sheet.cursor_row) || !writer.i32(sheet.cursor_col) ||
        !writer.i32(sheet.scroll_row) || !writer.i32(sheet.scroll_col) ||
        !writer.u8(sheet.freeze_row ? 1 : 0) || !writer.u8(sheet.freeze_column ? 1 : 0) ||
        !writer.u8(sheet.gridlines ? 1 : 0) || !writer.u8(sheet.filter_enabled ? 1 : 0) ||
        !writer.u8(sheet.filter_header ? 1 : 0) ||
        !writer.u32((uint32_t)sheet.column_widths.size())) return false;
    for (size_t i = 0; i < sheet.column_widths.size(); ++i)
        if (!writer.i32(sheet.column_widths[i])) return false;
    if (!writer.u32((uint32_t)sheet.row_heights.size())) return false;
    for (size_t i = 0; i < sheet.row_heights.size(); ++i)
        if (!writer.i32(sheet.row_heights[i])) return false;
    if (!write_range(writer, sheet.filter_range) || !writer.u32((uint32_t)sheet.filters.size()))
        return false;
    for (size_t i = 0; i < sheet.filters.size(); ++i) {
        const FilterTerm &filter = sheet.filters[i];
        if (!writer.i32(filter.column) || !writer.u8(filter.operation) ||
            !writer.string(filter.query)) return false;
    }
    if (!writer.u32((uint32_t)sheet.merges.size())) return false;
    for (size_t i = 0; i < sheet.merges.size(); ++i)
        if (!write_range(writer, sheet.merges[i])) return false;
    if (!writer.u32((uint32_t)sheet.cells.size())) return false;
    for (size_t i = 0; i < sheet.cells.size(); ++i) {
        const Cell &cell = sheet.cells[i];
        const Style &style = cell.style;
        if (!writer.i32(cell.row) || !writer.i32(cell.col) || !writer.u8(cell.literal ? 1 : 0) ||
            !writer.u32(style.foreground) || !writer.u32(style.background) ||
            !writer.u16(style.size) || !writer.u8(style.bold) || !writer.u8(style.italic) ||
            !writer.u8(style.wrap) || !writer.u8(style.borders) ||
            !writer.u8((uint8_t)style.alignment) || !writer.u8((uint8_t)style.format) ||
            !writer.string(cell.input)) return false;
    }
    return true;
}

bool serialize_workbook(FileWriter &writer, const Workbook &book, uint64_t file_size) {
    unsigned char header[16] = {'R','E','X','L',0,0,0,0,0,0,0,0,0,0,0,0};
    store_u16(header + 4, NativeVersion);
    store_u64(header + 8, file_size - sizeof(header));
    if (!writer.write(header, sizeof(header)) || !writer.u32(book.next_id) ||
        !writer.u32(book.active_id) || !writer.u64(book.revision) ||
        !writer.u32((uint32_t)book.sheets.size())) return false;
    for (size_t i = 0; i < book.sheets.size(); ++i)
        if (!write_sheet(writer, book.sheets[i])) return false;
    (void)file_size;
    return true;
}

struct FileReader {
    int fd = -1;
    uint64_t size = 0, consumed = 0, next_progress = 0;
    unsigned char buffer[WriteBufferSize];
    size_t buffer_begin = 0, buffer_end = 0;
    Progress progress = nullptr;
    void *context = nullptr;
    Text *error = nullptr;

    bool read(void *data, size_t count) {
        if ((uint64_t)count > size - consumed) {
            if (error) set_error(*error, "Workbook is truncated");
            return false;
        }
        unsigned char *at = (unsigned char *)data;
        size_t left = count;
        while (left) {
            if (buffer_begin == buffer_end) {
                buffer_begin = buffer_end = 0;
                uint64_t remaining = size - consumed;
                size_t request = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
                ssize_t got;
                do { got = ::read(fd, buffer, request); } while (got < 0 && errno == EINTR);
                if (got <= 0) {
                    if (error) {
                        if (!got) set_error(*error, "Workbook is truncated");
                        else set_errno_error(*error, "Could not read workbook");
                    }
                    return false;
                }
                buffer_end = (size_t)got;
            }
            size_t available = buffer_end - buffer_begin;
            size_t copied = left < available ? left : available;
            memcpy(at, buffer + buffer_begin, copied);
            buffer_begin += copied;
            at += copied;
            left -= copied;
            consumed += copied;
            if (progress && (consumed >= next_progress || consumed == size)) {
                if (!progress_call(progress, context,
                        (unsigned)(consumed > UINT_MAX ? UINT_MAX : consumed),
                        (unsigned)(size > UINT_MAX ? UINT_MAX : size))) {
                    if (error) set_error(*error, "Operation cancelled");
                    return false;
                }
                next_progress = consumed + IoProgressQuantum;
            }
        }
        return true;
    }
    bool u8(uint8_t &value) { return read(&value, 1); }
    bool u16(uint16_t &value) {
        unsigned char bytes[2]; if (!read(bytes, sizeof(bytes))) return false;
        value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8); return true;
    }
    bool u32(uint32_t &value) {
        unsigned char bytes[4]; if (!read(bytes, sizeof(bytes))) return false;
        value = load_u32(bytes); return true;
    }
    bool u64(uint64_t &value) {
        unsigned char bytes[8]; if (!read(bytes, sizeof(bytes))) return false;
        value = load_u64(bytes); return true;
    }
    bool i32(int &value) {
        uint32_t raw; if (!u32(raw)) return false;
        value = (int)(int32_t)raw; return true;
    }
    bool string(Text &value) {
        uint32_t length;
        if (!u32(length)) return false;
        if (length > MaxTextBytes || (uint64_t)length > size - consumed) {
            if (error) set_error(*error, "String exceeds workbook resource limits");
            return false;
        }
        char *buffer = (char *)malloc((size_t)length + 1);
        if (!buffer) {
            if (error) set_error(*error, "Not enough memory to read workbook");
            return false;
        }
        bool ok = read(buffer, length);
        if (ok) {
            buffer[length] = 0;
            if (!valid_bytes(buffer, length)) {
                ok = false;
                if (error) set_error(*error, "Workbook contains invalid UTF-8 text");
            } else if (!value.set(buffer, length)) {
                ok = false;
                if (error) set_error(*error, "Not enough memory to read workbook text");
            }
        }
        free(buffer);
        return ok;
    }
};

bool read_range(FileReader &reader, Range &range) {
    return reader.i32(range.r1) && reader.i32(range.c1) &&
        reader.i32(range.r2) && reader.i32(range.c2);
}

bool read_sheet(FileReader &reader, Sheet &sheet, uint64_t &cell_total,
                uint64_t &text_total) {
    if (!reader.u32(sheet.id) || !reader.string(sheet.name) || !reader.i32(sheet.zoom) ||
        !reader.i32(sheet.cursor_row) || !reader.i32(sheet.cursor_col) ||
        !reader.i32(sheet.scroll_row) || !reader.i32(sheet.scroll_col)) return false;
    uint8_t flags[5];
    for (unsigned i = 0; i < 5; ++i) if (!reader.u8(flags[i])) return false;
    for (unsigned i = 0; i < 5; ++i) if (flags[i] > 1) {
        if (reader.error) set_error(*reader.error, "Invalid worksheet flags");
        return false;
    }
    sheet.freeze_row = flags[0] != 0;
    sheet.freeze_column = flags[1] != 0;
    sheet.gridlines = flags[2] != 0;
    sheet.filter_enabled = flags[3] != 0;
    sheet.filter_header = flags[4] != 0;

    uint32_t count;
    if (!reader.u32(count) || count > (uint32_t)MaxCols || !sheet.column_widths.resize(count)) {
        if (reader.error && reader.error->empty()) set_error(*reader.error, "Invalid column dimensions");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) if (!reader.i32(sheet.column_widths[i])) return false;
    if (!reader.u32(count) || count > (uint32_t)MaxRows || !sheet.row_heights.resize(count)) {
        if (reader.error && reader.error->empty()) set_error(*reader.error, "Invalid row dimensions");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) if (!reader.i32(sheet.row_heights[i])) return false;
    if (!read_range(reader, sheet.filter_range) || !reader.u32(count) || count > (uint32_t)MaxCols) {
        if (reader.error && reader.error->empty()) set_error(*reader.error, "Invalid filter metadata");
        return false;
    }
    if (!sheet.filters.reserve(count)) {
        if (reader.error) set_error(*reader.error, "Not enough memory to read filters");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        FilterTerm filter;
        int column;
        uint8_t operation;
        if (!reader.i32(column) || !reader.u8(operation) || !reader.string(filter.query)) return false;
        filter.column = column;
        filter.operation = operation;
        if (!checked_add(text_total, filter.query.size())) {
            if (reader.error) set_error(*reader.error, "Workbook text exceeds the resource limit");
            return false;
        }
        if (!sheet.filters.push(static_cast<FilterTerm &&>(filter))) {
            if (reader.error) set_error(*reader.error, "Not enough memory to read filters");
            return false;
        }
    }
    if (!reader.u32(count) || count > MaxCells) {
        if (reader.error) set_error(*reader.error, "Invalid merged range count");
        return false;
    }
    if (!sheet.merges.reserve(count)) {
        if (reader.error) set_error(*reader.error, "Not enough memory to read merged ranges");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Range range;
        if (!read_range(reader, range) || !sheet.merges.push(range)) {
            if (reader.error && reader.error->empty()) set_error(*reader.error, "Invalid merged range");
            return false;
        }
    }
    if (!reader.u32(count) || count > MaxCells || count > MaxCells - cell_total) {
        if (reader.error) set_error(*reader.error, "Workbook exceeds the cell limit");
        return false;
    }
    if (!sheet.cells.reserve(count)) {
        if (reader.error) set_error(*reader.error, "Not enough memory to read cells");
        return false;
    }
    int previous = -1;
    for (uint32_t i = 0; i < count; ++i) {
        int row, column;
        uint8_t literal, bold, italic, wrap, borders, alignment, format;
        uint32_t foreground, background;
        uint16_t font_size;
        Text input;
        if (!reader.i32(row) || !reader.i32(column) || !reader.u8(literal) ||
            !reader.u32(foreground) || !reader.u32(background) || !reader.u16(font_size) ||
            !reader.u8(bold) || !reader.u8(italic) || !reader.u8(wrap) ||
            !reader.u8(borders) || !reader.u8(alignment) || !reader.u8(format) ||
            !reader.string(input)) return false;
        if (row < 0 || row >= MaxRows || column < 0 || column >= MaxCols || literal > 1 ||
            bold > 1 || italic > 1 || wrap > 1 || borders > 15 || alignment > (uint8_t)Alignment::Right ||
            format > (uint8_t)NumberFormat::Currency || font_size < 6 || font_size > 96) {
            if (reader.error) set_error(*reader.error, "Invalid cell coordinate or style");
            return false;
        }
        int key = row * MaxCols + column;
        if (key <= previous) {
            if (reader.error) set_error(*reader.error, "Duplicate or unordered cell coordinate");
            return false;
        }
        previous = key;
        if (!checked_add(text_total, input.size())) {
            if (reader.error) set_error(*reader.error, "Workbook text exceeds the resource limit");
            return false;
        }
        if (!sheet.set_input(row, column, input.c_str(), literal != 0)) {
            if (reader.error) set_error(*reader.error, "Not enough memory to read cell input");
            return false;
        }
        Style style;
        style.foreground = foreground;
        style.background = background;
        style.size = font_size;
        style.bold = bold;
        style.italic = italic;
        style.wrap = wrap;
        style.borders = borders;
        style.alignment = (Alignment)alignment;
        style.format = (NumberFormat)format;
        if (!sheet.set_style(row, column, style)) {
            if (reader.error) set_error(*reader.error, "Not enough memory to read cell style");
            return false;
        }
    }
    cell_total += count;
    return true;
}

bool read_workbook(FileReader &reader, Workbook &book) {
    uint32_t sheet_count;
    if (!reader.u32(book.next_id) || !reader.u32(book.active_id) ||
        !reader.u64(book.revision) || !reader.u32(sheet_count) ||
        !sheet_count || sheet_count > (uint32_t)MaxSheets) {
        if (reader.error && reader.error->empty()) set_error(*reader.error, "Invalid workbook metadata");
        return false;
    }
    if (!book.sheets.reserve(sheet_count)) {
        if (reader.error) set_error(*reader.error, "Not enough memory to read worksheets");
        return false;
    }
    uint64_t cell_total = 0, text_total = 0;
    for (uint32_t i = 0; i < sheet_count; ++i) {
        Sheet sheet;
        if (!read_sheet(reader, sheet, cell_total, text_total)) return false;
        if (!book.sheets.push(static_cast<Sheet &&>(sheet))) {
            if (reader.error) set_error(*reader.error, "Not enough memory to read worksheets");
            return false;
        }
    }
    if (reader.consumed != reader.size) {
        if (reader.error) set_error(*reader.error, "Workbook has trailing or malformed data");
        return false;
    }
    uint64_t estimated;
    if (!validate_workbook(book, estimated, *reader.error)) return false;
    return true;
}

bool target_metadata(const char *path, struct stat &st, bool &exists, Text &error) {
    exists = false;
    if (lstat(path, &st) == 0) {
        exists = true;
        if (!S_ISREG(st.st_mode)) return set_error(error, "Save target must be a regular file");
        return true;
    }
    if (errno == ENOENT) return true;
    return set_errno_error(error, "Could not inspect save target");
}

bool check_expected_stamp(const char *path, const FileStamp *expected, Text &error) {
    if (!expected) return true;
    FileStamp current;
    if (!file_stamp(path, current, error)) return false;
    if (!same_stamp(*expected, current))
        return set_error(error, "The file changed outside ReliefExcel; the original was preserved");
    return true;
}

bool make_temp_path(const char *path, char **temporary, Text &error) {
    static uint32_t serial;
    size_t length = strlen(path);
    if (length > 4000) return set_error(error, "Save path is too long");
    char *result = (char *)malloc(length + 64);
    if (!result) return set_error(error, "Not enough memory to save workbook");
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        uint32_t id = ++serial;
        int n = snprintf(result, length + 64, "%s.tmp.%ld.%u", path, (long)getpid(), id);
        if (n > 0 && (size_t)n < length + 64) {
            *temporary = result;
            return true;
        }
    }
    free(result);
    return set_error(error, "Could not create temporary save path");
}

bool sync_parent_directory(const char *path, Text &error) {
    size_t length = strlen(path);
    if (length > 4000) return set_error(error, "Save path is too long");
    char *directory = (char *)malloc(length + 2);
    if (!directory) return set_error(error, "Not enough memory to sync save directory");
    memcpy(directory, path, length + 1);
    char *slash = strrchr(directory, '/');
    if (!slash) strcpy(directory, ".");
    else if (slash == directory) slash[1] = 0;
    else *slash = 0;
#ifdef O_DIRECTORY
    int fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
#else
    int fd = open(directory, O_RDONLY | O_CLOEXEC);
#endif
    free(directory);
    if (fd < 0) return set_errno_error(error, "Could not open save directory");
    bool ok = fsync(fd) == 0;
    int saved = errno;
    close(fd);
    if (!ok) { errno = saved; return set_errno_error(error, "Could not sync save directory"); }
    return true;
}

bool write_atomic(const char *path, const Workbook *book, uint32_t sheet_id,
                  const Range *range, Text &error, bool native, bool bom,
                  const FileStamp *expected, Progress progress, void *context);

struct ByteBuffer {
    char *data = nullptr;
    size_t size = 0, capacity = 0;
    ~ByteBuffer() { free(data); }
    bool reserve(size_t requested) {
        if (requested <= capacity) return true;
        if ((uint64_t)requested > MaxDelimitedBytes) return false;
        size_t next = capacity ? capacity : 128;
        while (next < requested) {
            if (next > SIZE_MAX / 2) { next = requested; break; }
            next *= 2;
        }
        if ((uint64_t)next > MaxDelimitedBytes) next = requested;
        char *grown = (char *)realloc(data, next);
        if (!grown) return false;
        data = grown;
        capacity = next;
        return true;
    }
    bool append(const void *bytes, size_t length) {
        if (length > SIZE_MAX - size || !reserve(size + length)) return false;
        if (length) memcpy(data + size, bytes, length);
        size += length;
        return true;
    }
    bool byte(char value) { return append(&value, 1); }
};

bool append_utf8_field(ByteBuffer &output, const char *value, size_t length,
                       char delimiter) {
    bool quoted = false;
    for (size_t i = 0; i < length; ++i)
        if (value[i] == delimiter || value[i] == '"' || value[i] == '\r' || value[i] == '\n')
            quoted = true;
    if (!quoted) return output.append(value, length);
    if (!output.byte('"')) return false;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] == '"' && !output.byte('"')) return false;
        if (!output.byte(value[i])) return false;
    }
    return output.byte('"');
}

bool valid_delimiter(char delimiter) {
    return delimiter && delimiter != '"' && delimiter != '\r' && delimiter != '\n';
}

bool numeric_csv_value(const char *field, size_t length, double &number) {
    if (!length || length > MaxTextBytes) return false;
    size_t at = 0;
    if (field[at] == '+' || field[at] == '-') ++at;
    if (at == length) return false;
    size_t integer_start = at, integer_digits = 0;
    while (at < length && field[at] >= '0' && field[at] <= '9') {
        ++at; ++integer_digits;
    }
    size_t fraction_digits = 0;
    if (at < length && field[at] == '.') {
        ++at;
        while (at < length && field[at] >= '0' && field[at] <= '9') {
            ++at; ++fraction_digits;
        }
    }
    if (!integer_digits && !fraction_digits) return false;
    if (at < length && (field[at] == 'e' || field[at] == 'E')) {
        ++at;
        if (at < length && (field[at] == '+' || field[at] == '-')) ++at;
        size_t exponent_digits = 0;
        while (at < length && field[at] >= '0' && field[at] <= '9') {
            ++at; ++exponent_digits;
        }
        if (!exponent_digits) return false;
    }
    if (at != length) return false;
    if (integer_digits > 1 && field[integer_start] == '0') return false;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return false;
    memcpy(copy, field, length);
    copy[length] = 0;
    locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!locale) { free(copy); return false; }
    errno = 0;
    char *end = nullptr;
    double parsed = strtod_l(copy, &end, locale);
    freelocale(locale);
    bool valid = end == copy + length && errno != ERANGE && isfinite(parsed);
    free(copy);
    if (!valid) return false;
    number = parsed;
    return true;
}

bool assign_csv_field(Sheet &sheet, int row, int column, const ByteBuffer &field,
                      uint64_t &cell_total, Text &error) {
    if (!valid_bytes(field.data ? field.data : "", field.size))
        return set_error(error, "Delimited text contains invalid UTF-8 or a NUL byte");
    if (!field.size) return true;
    if (cell_total >= MaxCells) return set_error(error, "Import exceeds the cell limit");
    Text input;
    if (!input.set(field.data, field.size)) return set_error(error, "Not enough memory to import worksheet cell");
    double number;
    bool is_number = numeric_csv_value(field.data, field.size, number);
    if (!sheet.set_input(row, column, input.c_str(), !is_number))
        return set_error(error, "Not enough memory to import worksheet cell");
    if (is_number) {
        Cell *cell = sheet.get(row, column);
        if (cell) cell->result = Value::numeric(number);
    }
    ++cell_total;
    return true;
}

bool decode_impl(const char *bytes, size_t length, Workbook &output, Text &error,
                 char delimiter, Progress progress, void *context) {
    if (!valid_delimiter(delimiter)) return set_error(error, "Invalid field delimiter");
    if ((!bytes && length) || (uint64_t)length > MaxDelimitedBytes)
        return set_error(error, "Delimited input exceeds the resource limit");
    Workbook candidate;
    if (!candidate.init()) return set_error(error, "Not enough memory to import worksheet");
    Sheet &sheet = candidate.sheets[0];
    if (length >= 3 && (unsigned char)bytes[0] == 0xef &&
        (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf) {
        bytes += 3;
        length -= 3;
    }
    size_t position = 0;
    int row = 0, column = 0;
    uint64_t cell_total = 0;
    bool record_just_ended = false;
    if (!progress_call(progress, context, 0, (unsigned)(length > UINT_MAX ? UINT_MAX : length)))
        return set_error(error, "Operation cancelled");
    size_t next_progress = IoProgressQuantum;
    size_t last_progress = 0;
    auto report_parse_progress = [&](bool force) {
        if (!progress || (!force && position < next_progress) || position == last_progress)
            return true;
        if (!progress_call(progress, context,
                (unsigned)(position > UINT_MAX ? UINT_MAX : position),
                (unsigned)(length > UINT_MAX ? UINT_MAX : length))) return false;
        last_progress = position;
        next_progress = position + IoProgressQuantum;
        return true;
    };
    while (position < length || (position == length && column > 0 && !record_just_ended)) {
        ByteBuffer field;
        bool quoted = false;
        if (position < length && bytes[position] == '"') {
            quoted = true;
            ++position;
            bool closed = false;
            while (position < length) {
                char current = bytes[position++];
                if (current != '"') {
                    if (!field.byte(current)) return set_error(error, "Delimited field exceeds resource limits");
                    if (field.size > MaxTextBytes)
                        return set_error(error, "Delimited field exceeds the text limit");
                } else if (position < length && bytes[position] == '"') {
                    ++position;
                    if (!field.byte('"')) return set_error(error, "Delimited field exceeds resource limits");
                    if (field.size > MaxTextBytes)
                        return set_error(error, "Delimited field exceeds the text limit");
                } else {
                    closed = true;
                    break;
                }
                if (!report_parse_progress(false))
                    return set_error(error, "Operation cancelled");
            }
            if (!closed) return set_error(error, "Unterminated quoted field");
            if (position < length && bytes[position] != delimiter && bytes[position] != '\n' && bytes[position] != '\r')
                return set_error(error, "Unexpected data after quoted field");
        } else {
            while (position < length && bytes[position] != delimiter &&
                   bytes[position] != '\n' && bytes[position] != '\r') {
                if (bytes[position] == '"') return set_error(error, "Unexpected quote in unquoted field");
                if (!field.byte(bytes[position++])) return set_error(error, "Delimited field exceeds resource limits");
                if (field.size > MaxTextBytes)
                    return set_error(error, "Delimited field exceeds the text limit");
                if (!report_parse_progress(false))
                    return set_error(error, "Operation cancelled");
            }
        }
        if (!report_parse_progress(false)) return set_error(error, "Operation cancelled");
        (void)quoted;
        if (row >= MaxRows || column >= MaxCols)
            return set_error(error, "Delimited input exceeds worksheet dimensions");
        if (!assign_csv_field(sheet, row, column, field, cell_total, error)) return false;
        record_just_ended = false;
        if (position == length) break;
        if (bytes[position] == delimiter) {
            ++position;
            ++column;
            if (position == length) {
                ByteBuffer empty;
                if (column >= MaxCols || !assign_csv_field(sheet, row, column, empty, cell_total, error))
                    return set_error(error, "Delimited input exceeds worksheet dimensions");
                break;
            }
            continue;
        }
        if (bytes[position] == '\r') {
            if (position + 1 >= length || bytes[position + 1] != '\n')
                return set_error(error, "Bare carriage return outside quoted field");
            position += 2;
        } else {
            ++position;
        }
        ++row;
        column = 0;
        record_just_ended = true;
        if (position == length) break;
        record_just_ended = false;
        if (!report_parse_progress(true))
            return set_error(error, "Operation cancelled");
    }
    if (row >= MaxRows && length && bytes[length - 1] != '\n')
        return set_error(error, "Delimited input exceeds worksheet dimensions");
    if (!recalculate(candidate, progress, context))
        return set_error(error, "Could not calculate imported worksheet");
    output.sheets.swap(candidate.sheets);
    output.next_id = candidate.next_id;
    output.active_id = candidate.active_id;
    output.revision = candidate.revision;
    return true;
}

bool get_value_text(Workbook &book, uint32_t sheet_id, int row, int column,
                    Text &value, Text &error) {
    const Cell *cell = book.get(sheet_id, row, column);
    if (!cell) { value.clear(); return true; }
    Value result = evaluate(book, sheet_id, row, column);
    value = format_value(result, Style{});
    if (value.size() > MaxTextBytes || !valid_bytes(value.c_str(), value.size()))
        return set_error(error, "Calculated value exceeds CSV text limits");
    return true;
}

bool encode_range(Workbook &book, uint32_t sheet_id, Range range, Text &output,
                  Text &error, char delimiter, Progress progress, void *context) {
    if (!valid_delimiter(delimiter) || !range.valid())
        return set_error(error, "Invalid delimited range");
    uint64_t area = (uint64_t)(range.r2 - range.r1 + 1) * (uint64_t)(range.c2 - range.c1 + 1);
    if (area > 2ULL * MaxRows * MaxCols) return set_error(error, "Export range exceeds resource limits");
    if (!book.sheet(sheet_id)) return set_error(error, "Worksheet no longer exists");
    ByteBuffer buffer;
    uint64_t total_rows = (uint64_t)(range.r2 - range.r1 + 1);
    if (!progress_call(progress, context, 0, (unsigned)(total_rows > UINT_MAX ? UINT_MAX : total_rows)))
        return set_error(error, "Operation cancelled");
    for (int row = range.r1; row <= range.r2; ++row) {
        for (int column = range.c1; column <= range.c2; ++column) {
            if (column != range.c1 && !buffer.byte(delimiter))
                return set_error(error, "Delimited output exceeds resource limits");
            Text field;
            if (!get_value_text(book, sheet_id, row, column, field, error)) return false;
            if (!append_utf8_field(buffer, field.c_str(), field.size(), delimiter))
                return set_error(error, "Delimited output exceeds resource limits");
        }
        if (!buffer.append("\r\n", 2)) return set_error(error, "Delimited output exceeds resource limits");
        if (progress && !progress_call(progress, context, (unsigned)(row - range.r1 + 1),
                                       (unsigned)total_rows))
            return set_error(error, "Operation cancelled");
    }
    if (!output.set(buffer.data ? buffer.data : "", buffer.size))
        return set_error(error, "Not enough memory to build delimited output");
    return true;
}

bool create_directories(const char *path, Text &error) {
    size_t length = strlen(path);
    if (!length || path[0] != '/' || length > 4000)
        return set_error(error, "State directory must be an absolute path");
    char *copy = (char *)malloc(length + 1);
    if (!copy) return set_error(error, "Not enough memory to create recovery directory");
    memcpy(copy, path, length + 1);
    for (size_t i = 1; i <= length; ++i) {
        if (copy[i] && copy[i] != '/') continue;
        if (!copy[i] && i == 1) continue;
        char saved = copy[i];
        copy[i] = 0;
        struct stat st;
        if (stat(copy, &st) < 0) {
            if (errno != ENOENT || mkdir(copy, 0700) < 0) {
                if (errno == EEXIST && stat(copy, &st) == 0 && S_ISDIR(st.st_mode)) {
                    copy[i] = saved;
                    continue;
                }
                copy[i] = saved;
                free(copy);
                return set_errno_error(error, "Could not create recovery directory");
            }
        } else if (!S_ISDIR(st.st_mode)) {
            copy[i] = saved;
            free(copy);
            return set_error(error, "Recovery path contains a non-directory component");
        }
        copy[i] = saved;
    }
    free(copy);
    return true;
}

bool private_directory(const char *path, Text &error) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno != ENOENT || mkdir(path, 0700) < 0)
            return set_errno_error(error, "Could not create recovery directory");
        if (lstat(path, &st) < 0) return set_errno_error(error, "Could not inspect recovery directory");
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return set_error(error, "Recovery path is not a directory");
    if (st.st_uid != geteuid()) return set_error(error, "Recovery directory belongs to another user");
    if (chmod(path, 0700) < 0) return set_errno_error(error, "Could not secure recovery directory");
    return true;
}

bool write_atomic(const char *path, const Workbook *book, uint32_t sheet_id,
                  const Range *range, Text &error, bool native, bool bom,
                  const FileStamp *expected, Progress progress, void *context) {
    if (!path || !*path) return set_error(error, "Save path is empty");
    uint64_t total = 0;
    Text delimited;
    if (native) {
        if (!book || !validate_workbook(*book, total, error)) return false;
    } else {
        if (!book) return set_error(error, "Worksheet is unavailable");
        if (range && !encode_range(*const_cast<Workbook *>(book), sheet_id, *range,
                                   delimited, error, ',', progress, context)) return false;
        total = delimited.size() + (bom ? 3 : 0);
        if (total > MaxDelimitedBytes + 3) return set_error(error, "Delimited output exceeds the resource limit");
    }
    if (!check_expected_stamp(path, expected, error)) return false;
    struct stat old_stat;
    bool old_exists;
    if (!target_metadata(path, old_stat, old_exists, error)) return false;
    if (progress && !progress_call(progress, context, 0,
                                   (unsigned)(total > UINT_MAX ? UINT_MAX : total)))
        return set_error(error, "Operation cancelled");

    char *temporary = nullptr;
    if (!make_temp_path(path, &temporary, error)) return false;
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(temporary, flags, 0600);
    if (fd < 0) {
        set_errno_error(error, "Could not create temporary save file");
        free(temporary);
        return false;
    }
    bool temp_exists = true;
    bool ok = true;
    if (old_exists && fchmod(fd, old_stat.st_mode & 0777) < 0) {
        set_errno_error(error, "Could not preserve workbook permissions");
        ok = false;
    }
    FileWriter writer;
    writer.fd = fd;
    writer.total = total;
    writer.next_progress = IoProgressQuantum;
    writer.progress = progress;
    writer.context = context;
    writer.error = &error;
    if (ok && native) ok = serialize_workbook(writer, *book, total);
    else if (ok) {
        if (bom) {
            static const unsigned char utf8_bom[3] = {0xef, 0xbb, 0xbf};
            ok = writer.write(utf8_bom, sizeof(utf8_bom));
        }
        if (ok && delimited.size()) ok = writer.write(delimited.c_str(), delimited.size());
    }
    if (ok) ok = writer.flush();
    if (ok && writer.written != total) ok = set_error(error, "Incomplete temporary save file");
    if (ok && fsync(fd) < 0) { set_errno_error(error, "Could not sync temporary save file"); ok = false; }
    if (close(fd) < 0 && ok) { set_errno_error(error, "Could not close temporary save file"); ok = false; }
    if (ok && !check_expected_stamp(path, expected, error)) ok = false;
    struct stat latest;
    bool latest_exists = false;
    if (ok && !target_metadata(path, latest, latest_exists, error)) ok = false;
    if (ok && rename(temporary, path) < 0) { set_errno_error(error, "Could not replace saved file"); ok = false; }
    else if (ok) temp_exists = false;
    if (ok && !sync_parent_directory(path, error)) ok = false;
    if (temp_exists) unlink(temporary);
    free(temporary);
    if (ok) error.clear();
    return ok;
}

} // namespace

bool file_stamp(const char *path, FileStamp &stamp, Text &error) {
    stamp = FileStamp{};
    if (!path || !*path) return set_error(error, "File path is empty");
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) return true;
        return set_errno_error(error, "Could not inspect file");
    }
    if (!S_ISREG(st.st_mode)) return set_error(error, "File stamp requires a regular file");
    stamp.exists = true;
    stamp.device = (uint64_t)st.st_dev;
    stamp.inode = (uint64_t)st.st_ino;
    stamp.size = (uint64_t)st.st_size;
#if defined(__APPLE__)
    stamp.seconds = (int64_t)st.st_mtimespec.tv_sec;
    stamp.nanoseconds = (int64_t)st.st_mtimespec.tv_nsec;
    stamp.change_seconds = (int64_t)st.st_ctimespec.tv_sec;
    stamp.change_nanoseconds = (int64_t)st.st_ctimespec.tv_nsec;
#else
    stamp.seconds = (int64_t)st.st_mtim.tv_sec;
    stamp.nanoseconds = (int64_t)st.st_mtim.tv_nsec;
    stamp.change_seconds = (int64_t)st.st_ctim.tv_sec;
    stamp.change_nanoseconds = (int64_t)st.st_ctim.tv_nsec;
#endif
    return true;
}

bool same_stamp(const FileStamp &a, const FileStamp &b) {
    return a.exists == b.exists && (!a.exists ||
        (a.device == b.device && a.inode == b.inode && a.size == b.size &&
         a.seconds == b.seconds && a.nanoseconds == b.nanoseconds &&
         a.change_seconds == b.change_seconds &&
         a.change_nanoseconds == b.change_nanoseconds));
}

bool load_workbook(const char *path, Workbook &output, Text &error,
                   Progress progress, void *context) {
    if (!path || !*path) return set_error(error, "Workbook path is empty");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return set_errno_error(error, "Could not open workbook");
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int saved = errno; close(fd); errno = saved;
        return set_errno_error(error, "Could not inspect workbook");
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 16 || (uint64_t)st.st_size > MaxDocumentBytes) {
        close(fd);
        return set_error(error, "Workbook file has an invalid size or type");
    }
    FileReader reader;
    reader.fd = fd;
    reader.size = (uint64_t)st.st_size;
    reader.progress = progress;
    reader.context = context;
    reader.error = &error;
    if (!progress_call(progress, context, 0, (unsigned)reader.size)) {
        close(fd);
        return set_error(error, "Operation cancelled");
    }
    unsigned char header[16];
    if (!reader.read(header, sizeof(header))) { close(fd); return false; }
    uint16_t version = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
    uint16_t flags = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    uint64_t payload = load_u64(header + 8);
    if (memcmp(header, "REXL", 4) || version != NativeVersion || flags ||
        payload != reader.size - sizeof(header)) {
        close(fd);
        return set_error(error, version != NativeVersion ? "Unsupported workbook version" :
                         "Invalid workbook header or length");
    }
    Workbook candidate;
    if (!read_workbook(reader, candidate)) { close(fd); return false; }
    close(fd);
    if (!recalculate(candidate, progress, context))
        return set_error(error, "Could not calculate workbook or operation cancelled");
    output.sheets.swap(candidate.sheets);
    output.next_id = candidate.next_id;
    output.active_id = candidate.active_id;
    output.revision = candidate.revision;
    error.clear();
    return true;
}

bool save_workbook(const char *path, const Workbook &book, Text &error,
                   const FileStamp *expected, Progress progress, void *context) {
    return write_atomic(path, &book, 0, nullptr, error, true, false, expected, progress, context);
}

bool import_csv(const char *path, Workbook &output, Text &error, char delimiter,
                Progress progress, void *context) {
    if (!path || !*path) return set_error(error, "Delimited file path is empty");
    if (!valid_delimiter(delimiter)) return set_error(error, "Invalid field delimiter");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return set_errno_error(error, "Could not open delimited file");
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int saved = errno; close(fd); errno = saved;
        return set_errno_error(error, "Could not inspect delimited file");
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > MaxDelimitedBytes) {
        close(fd);
        return set_error(error, "Delimited file has an invalid size or type");
    }
    size_t length = (size_t)st.st_size;
    char *bytes = (char *)malloc(length ? length : 1);
    if (!bytes) { close(fd); return set_error(error, "Not enough memory to read delimited file"); }
    size_t offset = 0;
    bool ok = progress_call(progress, context, 0, (unsigned)(length > UINT_MAX ? UINT_MAX : length));
    if (!ok) set_error(error, "Operation cancelled");
    while (ok && offset < length) {
        ssize_t got = ::read(fd, bytes + offset, length - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (!got) set_error(error, "Delimited file is truncated");
            else set_errno_error(error, "Could not read delimited file");
            ok = false;
            break;
        }
        offset += (size_t)got;
        if (progress && !progress_call(progress, context,
                (unsigned)(offset > UINT_MAX ? UINT_MAX : offset),
                (unsigned)(length > UINT_MAX ? UINT_MAX : length))) {
            set_error(error, "Operation cancelled");
            ok = false;
        }
    }
    close(fd);
    if (ok) ok = decode_impl(bytes, length, output, error, delimiter, progress, context);
    free(bytes);
    if (ok) error.clear();
    return ok;
}

bool export_csv(const char *path, Workbook &book, uint32_t sheet_id, Text &error,
                bool bom, Progress progress, void *context, const FileStamp *expected) {
    Sheet *sheet = book.sheet(sheet_id);
    if (!sheet) return set_error(error, "Worksheet no longer exists");
    int rows = sheet->used_rows();
    int columns = sheet->used_columns();
    Range range;
    const Range *selected = nullptr;
    if (rows > 0 && columns > 0) {
        range = {0, 0, rows - 1, columns - 1};
        selected = &range;
    }
    bool result = write_atomic(path, &book, sheet_id, selected, error, false,
                               bom, expected, progress, context);
    if (result) error.clear();
    return result;
}

bool decode_delimited(const char *bytes, size_t length, Workbook &output, Text &error,
                      char delimiter) {
    bool result = decode_impl(bytes, length, output, error, delimiter, nullptr, nullptr);
    if (result) error.clear();
    return result;
}

bool encode_delimited(Workbook &book, uint32_t sheet, Range range, Text &output,
                      Text &error, char delimiter) {
    bool result = encode_range(book, sheet, range, output, error, delimiter, nullptr, nullptr);
    if (result) error.clear();
    return result;
}

bool recovery_directory(Text &output, Text &error) {
    const char *base = getenv("XDG_STATE_HOME");
    char *fallback = nullptr;
    if (!base || !*base || base[0] != '/') {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') return set_error(error, "No per-user state directory is available");
        size_t n = strlen(home);
        if (n > 3900) return set_error(error, "State directory path is too long");
        fallback = (char *)malloc(n + sizeof("/.local/state"));
        if (!fallback) return set_error(error, "Not enough memory to locate recovery directory");
        snprintf(fallback, n + sizeof("/.local/state"), "%s/.local/state", home);
        base = fallback;
    }
    if (!create_directories(base, error)) { free(fallback); return false; }
    size_t n = strlen(base);
    if (n > 3900) { free(fallback); return set_error(error, "State directory path is too long"); }
    char *app = (char *)malloc(n + sizeof("/reliefexcel"));
    char *recovery = (char *)malloc(n + sizeof("/reliefexcel/recovery"));
    if (!app || !recovery) {
        free(app); free(recovery); free(fallback);
        return set_error(error, "Not enough memory to locate recovery directory");
    }
    snprintf(app, n + sizeof("/reliefexcel"), "%s/reliefexcel", base);
    snprintf(recovery, n + sizeof("/reliefexcel/recovery"), "%s/reliefexcel/recovery", base);
    bool ok = private_directory(app, error) && private_directory(recovery, error);
    if (ok && !output.set(recovery)) ok = set_error(error, "Not enough memory to return recovery path");
    free(app); free(recovery); free(fallback);
    if (ok) error.clear();
    return ok;
}

bool reserve_recovery_path(Text &output, Text &error) {
    Text directory;
    if (!recovery_directory(directory, error)) return false;
    static const char temporary_suffix[] = "/session-XXXXXX";
    static const char final_suffix[] = "/session-XXXXXX.rexl";
    const size_t directory_length = directory.size();
    const size_t final_length = sizeof(final_suffix) - 1;
    if (directory_length > 4000 || final_length > 4000 - directory_length)
        return set_error(error, "State directory path is too long");
    const size_t capacity = directory_length + sizeof(final_suffix);
    char *temporary = (char *)malloc(capacity);
    char *reserved = (char *)malloc(capacity);
    if (!temporary || !reserved) {
        free(temporary);
        free(reserved);
        return set_error(error, "Not enough memory to reserve recovery file");
    }
    auto cleanup_files = [](int fd, const char *temporary_path,
                            const char *reserved_path, bool remove_reserved) {
        int failure = 0;
        if (fd >= 0 && close(fd) < 0) failure = errno;
        if (remove_reserved && unlink(reserved_path) < 0 && errno != ENOENT && !failure)
            failure = errno;
        if (temporary_path && unlink(temporary_path) < 0 && errno != ENOENT && !failure)
            failure = errno;
        return failure;
    };

    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        int n = snprintf(temporary, capacity, "%s%s", directory.c_str(), temporary_suffix);
        if (n < 0 || (size_t)n >= capacity) {
            free(temporary);
            free(reserved);
            return set_error(error, "State directory path is too long");
        }
        int fd = mkstemp(temporary);
        if (fd < 0) {
            int saved = errno;
            if (saved == EEXIST) continue;
            free(temporary);
            free(reserved);
            errno = saved;
            return set_errno_error(error, "Could not create recovery reservation");
        }

        int descriptor_flags = fcntl(fd, F_GETFD);
        int failure = descriptor_flags < 0 ? errno : 0;
        if (!failure && fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
            failure = errno;
        if (!failure && fchmod(fd, 0600) < 0) failure = errno;
        struct stat st;
        if (!failure && fstat(fd, &st) < 0) failure = errno;
        if (!failure && (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                         (st.st_mode & 0777) != 0600)) failure = EPERM;
        if (failure) {
            int saved = failure;
            int cleanup_failure = cleanup_files(fd, temporary, nullptr, false);
            free(temporary);
            free(reserved);
            errno = cleanup_failure ? cleanup_failure : saved;
            if (cleanup_failure)
                return set_errno_error(error, "Could not clean recovery reservation");
            return set_errno_error(error, "Could not secure recovery reservation");
        }
        if (close(fd) < 0) {
            int saved = errno;
            int cleanup_failure = cleanup_files(-1, temporary, nullptr, false);
            free(temporary);
            free(reserved);
            errno = cleanup_failure ? cleanup_failure : saved;
            if (cleanup_failure)
                return set_errno_error(error, "Could not clean recovery reservation");
            return set_errno_error(error, "Could not close recovery reservation");
        }

        size_t temporary_length = strlen(temporary);
        memcpy(reserved, temporary, temporary_length);
        memcpy(reserved + temporary_length, ".rexl", sizeof(".rexl"));
        if (link(temporary, reserved) < 0) {
            int saved = errno;
            int cleanup_failure = cleanup_files(-1, temporary, nullptr, false);
            if (cleanup_failure) {
                free(temporary);
                free(reserved);
                errno = cleanup_failure;
                return set_errno_error(error, "Could not clean recovery reservation");
            }
            if (saved == EEXIST) continue;
            free(temporary);
            free(reserved);
            errno = saved;
            return set_errno_error(error, "Could not reserve recovery file");
        }
        if (unlink(temporary) < 0) {
            int saved = errno;
            int cleanup_failure = cleanup_files(-1, temporary, reserved, true);
            free(temporary);
            free(reserved);
            errno = cleanup_failure ? cleanup_failure : saved;
            return set_errno_error(error, "Could not clean recovery reservation");
        }
        if (!output.set(reserved)) {
            int cleanup_failure = cleanup_files(-1, nullptr, reserved, true);
            free(temporary);
            free(reserved);
            if (cleanup_failure) {
                errno = cleanup_failure;
                return set_errno_error(error, "Could not clean recovery reservation");
            }
            return set_error(error, "Not enough memory to reserve recovery file");
        }
        free(temporary);
        free(reserved);
        error.clear();
        return true;
    }
    free(temporary);
    free(reserved);
    return set_error(error, "Could not reserve a unique recovery file");
}

bool lock_recovery_path(const char *path, int &descriptor, Text &error) {
    descriptor = -1;
    if (!path || !*path) return set_error(error, "Recovery path is empty");
    const size_t path_length = strlen(path);
    if (path_length > 3994) return set_error(error, "Recovery lock path is too long");
    char *lock_path = (char *)malloc(path_length + sizeof(".lock"));
    if (!lock_path) return set_error(error, "Not enough memory to build recovery lock path");
    memcpy(lock_path, path, path_length);
    memcpy(lock_path + path_length, ".lock", sizeof(".lock"));

    const int common_flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;
    int fd = open(lock_path, common_flags | O_CREAT | O_EXCL, 0600);
    bool created = fd >= 0;
    if (fd < 0 && errno == EEXIST) fd = open(lock_path, common_flags);
    if (fd < 0) {
        int saved_errno = errno;
        free(lock_path);
        errno = saved_errno;
        return set_errno_error(error, "Could not open recovery lock");
    }

    int failure = 0;
    const char *validation_error = nullptr;
    if (created && fchmod(fd, 0600) < 0) failure = errno;
    struct stat st;
    if (!failure && fstat(fd, &st) < 0) failure = errno;
    if (!failure && !S_ISREG(st.st_mode))
        validation_error = "Recovery lock is not a regular file";
    if (!failure && !validation_error && st.st_uid != geteuid())
        validation_error = "Recovery lock belongs to another user";
    if (!failure && !validation_error && (st.st_mode & 07777) != 0600)
        validation_error = "Recovery lock must have mode 0600";
    if (failure || validation_error) {
        close(fd);
        free(lock_path);
        if (validation_error) return set_error(error, validation_error);
        errno = failure;
        return set_errno_error(error, "Could not secure recovery lock");
    }

    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    int result;
    do {
        result = fcntl(fd, F_SETLK, &lock);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        int saved_errno = errno;
        close(fd);
        free(lock_path);
        errno = saved_errno;
        if (saved_errno == EACCES || saved_errno == EAGAIN)
            return set_error(error, "Recovery file is already in use");
        return set_errno_error(error, "Could not lock recovery file");
    }

    free(lock_path);
    descriptor = fd;
    error.clear();
    return true;
}

void unlock_recovery_path(int &descriptor) {
    int fd = descriptor;
    descriptor = -1;
    if (fd >= 0) close(fd);
}

} // namespace rx
