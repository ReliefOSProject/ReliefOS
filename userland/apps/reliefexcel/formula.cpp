#include "formula.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rx {
namespace {

struct Builder {
    char *bytes = nullptr;
    size_t size = 0, capacity = 0;
    bool good = true;

    ~Builder() { free(bytes); }
    bool append(const char *value, size_t length) {
        if (!good || length > SIZE_MAX - size - 1) return good = false;
        size_t needed = size + length + 1;
        if (needed > capacity) {
            size_t next = capacity ? capacity : 64;
            while (next < needed) {
                if (next > SIZE_MAX / 2) { next = needed; break; }
                next *= 2;
            }
            char *replacement = static_cast<char *>(realloc(bytes, next));
            if (!replacement) return good = false;
            bytes = replacement;
            capacity = next;
        }
        if (length) memcpy(bytes + size, value, length);
        size += length;
        bytes[size] = 0;
        return true;
    }
    bool append(const char *value) { return append(value, strlen(value)); }
    bool finish(Text &output) {
        if (!good) return false;
        if (!bytes) return output.set("", 0);
        return output.set(bytes, size);
    }
};

static bool ascii_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool ascii_digit(char c) { return c >= '0' && c <= '9'; }

static char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

static bool name_character(unsigned char c)
{
    return ascii_alpha(static_cast<char>(c)) || ascii_digit(static_cast<char>(c)) ||
        c == '_' || c == '.' || c >= 0x80;
}

static bool equal_name(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        if (ascii_lower(*left) != ascii_lower(*right)) return false;
        ++left;
        ++right;
    }
    return !*left && !*right;
}

static uint32_t lookup_sheet(const Workbook *book, const char *name)
{
    if (!book || !name) return 0;
    for (size_t i = 0; i < book->sheets.size(); ++i) {
        const Sheet &sheet = book->sheets[i];
        if (equal_name(sheet.name.c_str(), name)) return sheet.id;
    }
    return 0;
}

struct RefLex {
    size_t start = 0, end = 0;
    size_t prefix_start = 0, prefix_end = 0;
    size_t address_start = 0, address_end = 0;
    uint32_t sheet = 0;
    int row = 0, col = 0;
    bool qualified = false;
    bool row_absolute = false, col_absolute = false;
    bool valid = false;
    char name[256] = {};
};

static bool parse_reference(const char *source, size_t length, size_t &position,
                            uint32_t default_sheet, const Workbook *book,
                            RefLex &reference, bool require_boundary)
{
    size_t original = position;
    RefLex parsed;
    parsed.start = original;
    parsed.prefix_start = original;
    size_t cursor = original;

    if (cursor + 5 <= length && !memcmp(source + cursor, "#REF!", 5)) {
        parsed.end = cursor + 5;
        parsed.address_start = cursor;
        parsed.address_end = parsed.end;
        parsed.sheet = 0;
        parsed.valid = false;
        position = parsed.end;
        reference = parsed;
        return true;
    }

    bool qualified = false;
    char decoded_name[256];
    size_t decoded_size = 0;
    if (cursor < length && source[cursor] == '\'') {
        size_t p = cursor + 1;
        bool closed = false;
        while (p < length) {
            if (source[p] == '\'') {
                if (p + 1 < length && source[p + 1] == '\'') {
                    if (decoded_size + 1 >= sizeof(decoded_name)) return false;
                    decoded_name[decoded_size++] = '\'';
                    p += 2;
                    continue;
                }
                ++p;
                closed = true;
                break;
            }
            if (decoded_size + 1 >= sizeof(decoded_name)) return false;
            decoded_name[decoded_size++] = source[p++];
        }
        if (closed && p < length && source[p] == '!') {
            qualified = true;
            cursor = p + 1;
        } else {
            return false;
        }
    } else {
        size_t p = cursor;
        while (p < length && name_character(static_cast<unsigned char>(source[p]))) ++p;
        if (p > cursor && p < length && source[p] == '!') {
            size_t n = p - cursor;
            if (n >= sizeof(decoded_name)) return false;
            memcpy(decoded_name, source + cursor, n);
            decoded_size = n;
            qualified = true;
            cursor = p + 1;
        }
    }
    decoded_name[decoded_size] = 0;
    parsed.qualified = qualified;
    parsed.prefix_end = cursor;
    parsed.address_start = cursor;
    if (qualified) {
        memcpy(parsed.name, decoded_name, decoded_size + 1);
        parsed.sheet = lookup_sheet(book, parsed.name);
    } else {
        parsed.sheet = default_sheet;
    }

    if (cursor < length && source[cursor] == '$') {
        parsed.col_absolute = true;
        ++cursor;
    }
    int column = 0;
    unsigned column_letters = 0;
    while (cursor < length && ascii_alpha(source[cursor])) {
        unsigned letter = static_cast<unsigned>(ascii_upper(source[cursor]) - 'A' + 1);
        if (column > 1000000) return false;
        column = column * 26 + static_cast<int>(letter);
        ++column_letters;
        ++cursor;
    }
    if (!column_letters) return false;
    if (cursor < length && source[cursor] == '$') {
        parsed.row_absolute = true;
        ++cursor;
    }
    if (cursor >= length || !ascii_digit(source[cursor])) return false;
    unsigned long row = 0;
    while (cursor < length && ascii_digit(source[cursor])) {
        unsigned digit = static_cast<unsigned>(source[cursor] - '0');
        if (row > 100000000UL) return false;
        row = row * 10 + digit;
        ++cursor;
    }
    if (!row || row > static_cast<unsigned long>(INT_MAX)) return false;
    if (require_boundary && cursor < length &&
        (name_character(static_cast<unsigned char>(source[cursor])) || source[cursor] == '$'))
        return false;

    parsed.row = static_cast<int>(row - 1);
    parsed.col = column - 1;
    parsed.address_end = cursor;
    parsed.end = cursor;
    // A workbook-less caller is rewriting formula text, so it cannot resolve a
    // sheet name. Keep qualified references syntactically valid for that path;
    // evaluation still rejects names which do not resolve in its workbook.
    bool sheet_valid = qualified ? (parsed.sheet != 0 || !book) : (default_sheet != 0 || !book);
    parsed.valid = sheet_valid && parsed.row >= 0 && parsed.row < MaxRows &&
        parsed.col >= 0 && parsed.col < MaxCols;
    position = cursor;
    reference = parsed;
    return true;
}

static bool append_column(Builder &builder, int column)
{
    char reversed[16];
    size_t size = 0;
    unsigned value = static_cast<unsigned>(column + 1);
    if (column < 0) return false;
    while (value) {
        unsigned remainder = (value - 1) % 26;
        reversed[size++] = static_cast<char>('A' + remainder);
        value = (value - 1) / 26;
    }
    while (size) {
        char c = reversed[--size];
        if (!builder.append(&c, 1)) return false;
    }
    return true;
}

static bool append_address(Builder &builder, const RefLex &reference,
                           int row, int col)
{
    if (reference.col_absolute && !builder.append("$")) return false;
    if (!append_column(builder, col)) return false;
    if (reference.row_absolute && !builder.append("$")) return false;
    char digits[32];
    int written = snprintf(digits, sizeof(digits), "%d", row + 1);
    return written > 0 && static_cast<size_t>(written) < sizeof(digits) &&
        builder.append(digits, static_cast<size_t>(written));
}

static bool append_original_reference(Builder &builder, const char *source,
                                      const RefLex &reference, int row, int col,
                                      bool invalid)
{
    if (invalid) return builder.append("#REF!");
    if (!builder.append(source + reference.start,
                        reference.address_start - reference.start)) return false;
    if (!append_address(builder, reference, row, col)) return false;
    return true;
}

struct RefGroup {
    size_t start = 0, end = 0;
    RefLex first, last;
    bool range = false;
};

static bool parse_reference_group(const char *source, size_t length, size_t position,
                                  uint32_t default_sheet, const Workbook *book,
                                  RefGroup &group)
{
    size_t cursor = position;
    RefLex first;
    if (!parse_reference(source, length, cursor, default_sheet, book, first, true))
        return false;
    RefGroup parsed;
    parsed.start = position;
    parsed.first = first;
    parsed.last = first;
    parsed.end = cursor;
    if (cursor < length && source[cursor] == ':') {
        size_t second_position = cursor + 1;
        RefLex last;
        if (parse_reference(source, length, second_position,
                            first.qualified ? first.sheet : default_sheet,
                            book, last, true)) {
            parsed.range = true;
            parsed.last = last;
            parsed.end = second_position;
        }
    }
    group = parsed;
    return true;
}

static bool reference_is_function_name(const char *source, size_t length,
                                       const RefGroup &group)
{
    if (group.range || group.first.qualified) return false;
    size_t cursor = group.end;
    while (cursor < length && (source[cursor] == ' ' || source[cursor] == '\t' ||
                               source[cursor] == '\r' || source[cursor] == '\n')) ++cursor;
    return cursor < length && source[cursor] == '(';
}

static bool append_quoted_sheet(Builder &builder, const char *name)
{
    if (!builder.append("'")) return false;
    for (const char *p = name; *p; ++p) {
        if (*p == '\'' && !builder.append("'")) return false;
        if (!builder.append(p, 1)) return false;
    }
    return builder.append("'!");
}

static bool append_rewritten_name_ref(Builder &builder, const char *source,
                                      const RefLex &reference,
                                      const char *old_name, const char *new_name,
                                      bool deleting)
{
    if (!reference.qualified || !equal_name(reference.name, old_name))
        return builder.append(source + reference.start, reference.end - reference.start);
    if (deleting) return builder.append("#REF!");
    if (!append_quoted_sheet(builder, new_name)) return false;
    return builder.append(source + reference.address_start,
                          reference.address_end - reference.address_start);
}

static bool rewrite_formula_sheet_name(Text &output, const char *input,
                                       const char *old_name, const char *new_name)
{
    size_t length = strlen(input);
    Builder builder;
    size_t cursor = 0, copied = 0;
    bool deleting = new_name == nullptr;
    while (cursor < length) {
        if (input[cursor] == '"') {
            size_t end = cursor + 1;
            while (end < length) {
                if (input[end] == '"') {
                    if (end + 1 < length && input[end + 1] == '"') { end += 2; continue; }
                    ++end;
                    break;
                }
                ++end;
            }
            cursor = end;
            continue;
        }
        if (cursor && (name_character(static_cast<unsigned char>(input[cursor - 1])) ||
                       input[cursor - 1] == '!' || input[cursor - 1] == ']' ||
                       input[cursor - 1] == '[' || input[cursor - 1] == '$')) {
            ++cursor;
            continue;
        }
        RefGroup group;
        if (parse_reference_group(input, length, cursor, 0, nullptr, group)) {
            if (reference_is_function_name(input, length, group)) {
                cursor = group.end;
                continue;
            }
            if (!builder.append(input + copied, cursor - copied)) return false;
            bool first_match = group.first.qualified && equal_name(group.first.name, old_name);
            bool last_match = group.range && group.last.qualified &&
                equal_name(group.last.name, old_name);
            if (deleting && (first_match || last_match)) {
                if (!builder.append("#REF!")) return false;
            } else {
                if (!append_rewritten_name_ref(builder, input, group.first,
                                               old_name, new_name, deleting)) return false;
                if (group.range) {
                    if (!builder.append(":")) return false;
                    if (!append_rewritten_name_ref(builder, input, group.last,
                                                   old_name, new_name, deleting)) return false;
                }
            }
            cursor = group.end;
            copied = cursor;
            continue;
        }
        ++cursor;
    }
    if (!builder.append(input + copied, length - copied)) return false;
    return builder.finish(output);
}

static void translate_one(RefLex &reference, int row_delta, int col_delta,
                          int &row, int &col, bool &invalid)
{
    int64_t translated_row = static_cast<int64_t>(reference.row) +
        (reference.row_absolute ? 0 : static_cast<int64_t>(row_delta));
    int64_t translated_col = static_cast<int64_t>(reference.col) +
        (reference.col_absolute ? 0 : static_cast<int64_t>(col_delta));
    invalid = !reference.valid || translated_row < 0 || translated_row >= MaxRows ||
        translated_col < 0 || translated_col >= MaxCols;
    row = invalid ? reference.row : static_cast<int>(translated_row);
    col = invalid ? reference.col : static_cast<int>(translated_col);
}

static void transform_insert_interval(int &first, int &last, int at, int count)
{
    if (at <= first) {
        first += count;
        last += count;
    } else if (at <= last) {
        last += count;
    }
}

static bool transform_delete_interval(int &first, int &last, int at, int count)
{
    int end = at + count;
    int overlap_first = first > at ? first : at;
    int overlap_last = last < end - 1 ? last : end - 1;
    int removed = overlap_first <= overlap_last ? overlap_last - overlap_first + 1 : 0;
    int size = last - first + 1;
    if (removed >= size) return false;
    if (first >= end) first -= count;
    else if (first >= at) first = at;
    if (last >= end) last -= count;
    else if (last >= at) last = at - 1;
    return true;
}

static bool append_axis_group(Builder &builder, const char *source,
                              const RefGroup &group, uint32_t target_sheet,
                              bool rows, int first, int count, bool deleted,
                              uint32_t formula_sheet)
{
    uint32_t first_sheet = group.first.qualified ? group.first.sheet : formula_sheet;
    uint32_t last_sheet = group.last.qualified ? group.last.sheet :
        (group.first.qualified ? group.first.sheet : formula_sheet);
    bool first_target = first_sheet == target_sheet;
    bool last_target = group.range && last_sheet == target_sheet;
    if (!group.range || !first_target || !last_target) {
        int row = group.first.row, col = group.first.col;
        bool invalid = !group.first.valid;
        if (first_target) {
            if (rows) {
                if (deleted) {
                    if (row >= first && row < first + count) invalid = true;
                    else if (row >= first + count) row -= count;
                } else if (row >= first) row += count;
            } else {
                if (deleted) {
                    if (col >= first && col < first + count) invalid = true;
                    else if (col >= first + count) col -= count;
                } else if (col >= first) col += count;
            }
        }
        if (row < 0 || row >= MaxRows || col < 0 || col >= MaxCols) invalid = true;
        if (!append_original_reference(builder, source, group.first, row, col, invalid)) return false;
        if (group.range) {
            int last_row = group.last.row, last_col = group.last.col;
            bool last_invalid = !group.last.valid;
            if (last_target) {
                if (rows) {
                    if (deleted) {
                        if (last_row >= first && last_row < first + count) last_invalid = true;
                        else if (last_row >= first + count) last_row -= count;
                    } else if (last_row >= first) last_row += count;
                } else {
                    if (deleted) {
                        if (last_col >= first && last_col < first + count) last_invalid = true;
                        else if (last_col >= first + count) last_col -= count;
                    } else if (last_col >= first) last_col += count;
                }
            }
            if (last_row < 0 || last_row >= MaxRows || last_col < 0 || last_col >= MaxCols)
                last_invalid = true;
            if (!builder.append(":")) return false;
            if (!append_original_reference(builder, source, group.last,
                                           last_row, last_col, last_invalid)) return false;
        }
        return true;
    }

    int low = rows ? group.first.row : group.first.col;
    int high = rows ? group.last.row : group.last.col;
    if (low > high) {
        int swap = low; low = high; high = swap;
    }
    bool survive;
    if (deleted) survive = transform_delete_interval(low, high, first, count);
    else { transform_insert_interval(low, high, first, count); survive = true; }
    if (!survive) return builder.append("#REF!");
    int row1 = group.first.row, col1 = group.first.col;
    int row2 = group.last.row, col2 = group.last.col;
    if (rows) { row1 = low; row2 = high; }
    else { col1 = low; col2 = high; }
    if (row1 < 0 || row1 >= MaxRows || row2 < 0 || row2 >= MaxRows ||
        col1 < 0 || col1 >= MaxCols || col2 < 0 || col2 >= MaxCols)
        return builder.append("#REF!");
    if (!builder.append(source + group.first.start,
                        group.first.address_start - group.first.start)) return false;
    if (!append_address(builder, group.first, row1, col1) || !builder.append(":")) return false;
    if (!builder.append(source + group.last.start,
                        group.last.address_start - group.last.start)) return false;
    return append_address(builder, group.last, row2, col2);
}

static bool rewrite_formula_axis(Text &output, const char *input,
                                 uint32_t formula_sheet, uint32_t target_sheet,
                                 bool rows, int first, int count, bool deleted,
                                 const Workbook &book)
{
    size_t length = strlen(input);
    Builder builder;
    size_t cursor = 0, copied = 0;
    while (cursor < length) {
        if (input[cursor] == '"') {
            size_t end = cursor + 1;
            while (end < length) {
                if (input[end] == '"') {
                    if (end + 1 < length && input[end + 1] == '"') { end += 2; continue; }
                    ++end;
                    break;
                }
                ++end;
            }
            cursor = end;
            continue;
        }
        if (cursor && (name_character(static_cast<unsigned char>(input[cursor - 1])) ||
                       input[cursor - 1] == '!' || input[cursor - 1] == ']' ||
                       input[cursor - 1] == '[' || input[cursor - 1] == '$')) {
            ++cursor;
            continue;
        }
        RefGroup group;
        if (parse_reference_group(input, length, cursor, formula_sheet, &book, group)) {
            if (reference_is_function_name(input, length, group)) {
                cursor = group.end;
                continue;
            }
            if (!builder.append(input + copied, cursor - copied)) return false;
            if (!append_axis_group(builder, input, group, target_sheet, rows,
                                   first, count, deleted, formula_sheet)) return false;
            cursor = group.end;
            copied = cursor;
            continue;
        }
        ++cursor;
    }
    if (!builder.append(input + copied, length - copied)) return false;
    return builder.finish(output);
}

static bool append_move_address(Builder &builder, const RefLex &reference,
                                int row, int col, uint32_t referenced_sheet,
                                uint32_t output_sheet, const Workbook &book)
{
    if (reference.qualified || referenced_sheet != output_sheet) {
        const Sheet *sheet = book.sheet(referenced_sheet);
        if (!sheet || !append_quoted_sheet(builder, sheet->name.c_str())) return false;
    }
    return append_address(builder, reference, row, col);
}

static bool rewrite_formula_move(Text &output, const char *input,
                                 uint32_t formula_sheet, bool moving_formula,
                                 uint32_t source_sheet, Range source,
                                 uint32_t target_sheet, int target_row, int target_col,
                                 const Workbook &book, bool &changed, bool &unsupported)
{
    size_t length = strlen(input);
    Builder builder;
    size_t cursor = 0, copied = 0;
    const uint32_t output_sheet = moving_formula ? target_sheet : formula_sheet;
    while (cursor < length) {
        if (input[cursor] == '"') {
            size_t end = cursor + 1;
            while (end < length) {
                if (input[end] == '"') {
                    if (end + 1 < length && input[end + 1] == '"') { end += 2; continue; }
                    ++end;
                    break;
                }
                ++end;
            }
            cursor = end;
            continue;
        }
        if (cursor && (name_character(static_cast<unsigned char>(input[cursor - 1])) ||
                       input[cursor - 1] == '!' || input[cursor - 1] == ']' ||
                       input[cursor - 1] == '[' || input[cursor - 1] == '$')) {
            ++cursor;
            continue;
        }
        RefGroup group;
        if (!parse_reference_group(input, length, cursor, formula_sheet, &book, group)) {
            ++cursor;
            continue;
        }
        if (reference_is_function_name(input, length, group)) {
            cursor = group.end;
            continue;
        }

        Builder replacement;
        bool group_changed = false;
        uint32_t first_sheet = group.first.sheet;
        uint32_t last_sheet = group.range ? group.last.sheet : first_sheet;
        if (group.range && first_sheet != last_sheet &&
            (first_sheet == source_sheet || last_sheet == source_sheet)) {
            // Cross-sheet ranges do not have unambiguous move semantics in the
            // supported formula grammar. Fail the enclosing cut safely.
            unsupported = true;
            return true;
        }
        if (!group.range && first_sheet == source_sheet && group.first.valid &&
            source.contains(group.first.row, group.first.col)) {
            int moved_row = target_row + group.first.row - source.r1;
            int moved_col = target_col + group.first.col - source.c1;
            if (!append_move_address(replacement, group.first, moved_row, moved_col,
                                     target_sheet, output_sheet, book)) return false;
            group_changed = true;
        } else if (group.range && first_sheet == source_sheet && last_sheet == source_sheet &&
                   group.first.valid && group.last.valid) {
            const int min_row = group.first.row < group.last.row ? group.first.row : group.last.row;
            const int max_row = group.first.row > group.last.row ? group.first.row : group.last.row;
            const int min_col = group.first.col < group.last.col ? group.first.col : group.last.col;
            const int max_col = group.first.col > group.last.col ? group.first.col : group.last.col;
            const bool overlaps = min_row <= source.r2 && source.r1 <= max_row &&
                min_col <= source.c2 && source.c1 <= max_col;
            const bool contained = source.contains(group.first.row, group.first.col) &&
                source.contains(group.last.row, group.last.col);
            if (overlaps && !contained) {
                unsupported = true;
                return true;
            }
            if (contained) {
                const int first_row = target_row + group.first.row - source.r1;
                const int first_col = target_col + group.first.col - source.c1;
                const int last_row = target_row + group.last.row - source.r1;
                const int last_col = target_col + group.last.col - source.c1;
                if (!append_move_address(replacement, group.first, first_row, first_col,
                                         target_sheet, output_sheet, book) ||
                    !replacement.append(":") ||
                    !append_move_address(replacement, group.last, last_row, last_col,
                                         target_sheet, output_sheet, book)) return false;
                group_changed = true;
            } else if (moving_formula && source_sheet != target_sheet &&
                       (!group.first.qualified || !group.last.qualified)) {
                // An unqualified reference in a moved formula used to resolve
                // on the source sheet. Preserve that binding after the move.
                if (!append_move_address(replacement, group.first, group.first.row, group.first.col,
                                         source_sheet, output_sheet, book) ||
                    !replacement.append(":") ||
                    !append_move_address(replacement, group.last, group.last.row, group.last.col,
                                         source_sheet, output_sheet, book)) return false;
                group_changed = true;
            }
        } else if (!group.range && moving_formula && source_sheet != target_sheet &&
                   first_sheet == source_sheet && !group.first.qualified) {
            // Moving a formula to another sheet changes the meaning of its
            // unqualified references outside the moved block unless qualified.
            if (!append_move_address(replacement, group.first, group.first.row, group.first.col,
                                     source_sheet, output_sheet, book)) return false;
            group_changed = true;
        }

        if (group_changed) {
            if (!builder.append(input + copied, cursor - copied) ||
                !builder.append(replacement.bytes, replacement.size)) return false;
            changed = true;
            copied = group.end;
        }
        cursor = group.end;
    }
    if (!builder.append(input + copied, length - copied)) return false;
    return builder.finish(output);
}

enum NodeKind : uint8_t {
    NodeNumber, NodeText, NodeBoolean, NodeError, NodeReference, NodeRange,
    NodeUnaryPlus, NodeUnaryMinus, NodeAdd, NodeSubtract, NodeMultiply,
    NodeDivide, NodeConcat, NodeEqual, NodeNotEqual, NodeLess, NodeLessEqual,
    NodeGreater, NodeGreaterEqual, NodeFunction
};

enum FunctionKind : uint8_t {
    FunctionUnknown, FunctionSum, FunctionAverage, FunctionMin, FunctionMax,
    FunctionCount, FunctionCountA, FunctionIf, FunctionRound, FunctionAbs,
    FunctionCountIf, FunctionSumIf
};

struct Node {
    NodeKind kind = NodeNumber;
    int left = -1, right = -1, next = -1, first_argument = -1;
    unsigned argument_count = 0;
    double number = 0;
    FunctionKind function = FunctionUnknown;
    RefLex reference;
    Text text;
};

class Parser {
    Workbook &book_;
    uint32_t sheet_;
    const char *source_;
    size_t length_, position_ = 0;
    unsigned nesting_ = 0;
    Array<Node> &nodes_;
    const char *error_ = nullptr;

    void spaces() { while (position_ < length_ &&
        (source_[position_] == ' ' || source_[position_] == '\t' ||
         source_[position_] == '\r' || source_[position_] == '\n')) ++position_; }

    int add(Node &&node)
    {
        if (nodes_.size() >= static_cast<size_t>(INT_MAX) || !nodes_.push(static_cast<Node &&>(node))) {
            error_ = "#NUM!";
            return -1;
        }
        return static_cast<int>(nodes_.size() - 1);
    }

    int binary(NodeKind kind, int left, int right)
    {
        Node node;
        node.kind = kind;
        node.left = left;
        node.right = right;
        return add(static_cast<Node &&>(node));
    }

    int parse_comparison()
    {
        int left = parse_concat();
        if (left < 0) return -1;
        while (!error_) {
            spaces();
            NodeKind kind;
            size_t consume = 0;
            if (position_ + 1 < length_ && source_[position_] == '<' && source_[position_ + 1] == '>')
                { kind = NodeNotEqual; consume = 2; }
            else if (position_ + 1 < length_ && source_[position_] == '<' && source_[position_ + 1] == '=')
                { kind = NodeLessEqual; consume = 2; }
            else if (position_ + 1 < length_ && source_[position_] == '>' && source_[position_ + 1] == '=')
                { kind = NodeGreaterEqual; consume = 2; }
            else if (position_ < length_ && source_[position_] == '=')
                { kind = NodeEqual; consume = 1; }
            else if (position_ < length_ && source_[position_] == '<')
                { kind = NodeLess; consume = 1; }
            else if (position_ < length_ && source_[position_] == '>')
                { kind = NodeGreater; consume = 1; }
            else break;
            position_ += consume;
            int right = parse_concat();
            if (right < 0) return -1;
            left = binary(kind, left, right);
            if (left < 0) return -1;
        }
        return left;
    }

    int parse_concat()
    {
        int left = parse_add();
        if (left < 0) return -1;
        while (!error_) {
            spaces();
            if (position_ >= length_ || source_[position_] != '&') break;
            ++position_;
            int right = parse_add();
            if (right < 0) return -1;
            left = binary(NodeConcat, left, right);
            if (left < 0) return -1;
        }
        return left;
    }

    int parse_add()
    {
        int left = parse_multiply();
        if (left < 0) return -1;
        while (!error_) {
            spaces();
            if (position_ >= length_ || (source_[position_] != '+' && source_[position_] != '-')) break;
            char operation = source_[position_++];
            int right = parse_multiply();
            if (right < 0) return -1;
            left = binary(operation == '+' ? NodeAdd : NodeSubtract, left, right);
            if (left < 0) return -1;
        }
        return left;
    }

    int parse_multiply()
    {
        int left = parse_unary();
        if (left < 0) return -1;
        while (!error_) {
            spaces();
            if (position_ >= length_ || (source_[position_] != '*' && source_[position_] != '/')) break;
            char operation = source_[position_++];
            int right = parse_unary();
            if (right < 0) return -1;
            left = binary(operation == '*' ? NodeMultiply : NodeDivide, left, right);
            if (left < 0) return -1;
        }
        return left;
    }

    int parse_unary()
    {
        spaces();
        if (position_ < length_ && (source_[position_] == '+' || source_[position_] == '-')) {
            char operation = source_[position_++];
            int child = parse_unary();
            if (child < 0) return -1;
            Node node;
            node.kind = operation == '+' ? NodeUnaryPlus : NodeUnaryMinus;
            node.left = child;
            return add(static_cast<Node &&>(node));
        }
        return parse_primary();
    }

    static FunctionKind find_function(const char *name)
    {
        if (equal_name(name, "SUM")) return FunctionSum;
        if (equal_name(name, "AVERAGE")) return FunctionAverage;
        if (equal_name(name, "MIN")) return FunctionMin;
        if (equal_name(name, "MAX")) return FunctionMax;
        if (equal_name(name, "COUNT")) return FunctionCount;
        if (equal_name(name, "COUNTA")) return FunctionCountA;
        if (equal_name(name, "IF")) return FunctionIf;
        if (equal_name(name, "ROUND")) return FunctionRound;
        if (equal_name(name, "ABS")) return FunctionAbs;
        if (equal_name(name, "COUNTIF")) return FunctionCountIf;
        if (equal_name(name, "SUMIF")) return FunctionSumIf;
        return FunctionUnknown;
    }

    int parse_function(const char *name)
    {
        ++position_; // opening parenthesis
        if (++nesting_ > 128) { error_ = "#VALUE!"; return -1; }
        Node node;
        node.kind = NodeFunction;
        node.function = find_function(name);
        int function_index = add(static_cast<Node &&>(node));
        if (function_index < 0) { --nesting_; return -1; }
        spaces();
        if (position_ < length_ && source_[position_] == ')') {
            ++position_;
            --nesting_;
            return function_index;
        }
        int previous = -1;
        while (!error_) {
            int argument = parse_comparison();
            if (argument < 0) break;
            if (previous < 0) nodes_[static_cast<size_t>(function_index)].first_argument = argument;
            else nodes_[static_cast<size_t>(previous)].next = argument;
            previous = argument;
            ++nodes_[static_cast<size_t>(function_index)].argument_count;
            spaces();
            if (position_ >= length_) { error_ = "#VALUE!"; break; }
            if (source_[position_] == ')') { ++position_; break; }
            if (source_[position_] != ',') { error_ = "#VALUE!"; break; }
            ++position_;
            spaces();
            if (position_ >= length_ || source_[position_] == ')' || source_[position_] == ',') {
                error_ = "#VALUE!";
                break;
            }
        }
        --nesting_;
        return error_ ? -1 : function_index;
    }

    int parse_text()
    {
        ++position_;
        Builder builder;
        bool closed = false;
        while (position_ < length_) {
            if (source_[position_] == '"') {
                if (position_ + 1 < length_ && source_[position_ + 1] == '"') {
                    if (!builder.append("\"", 1)) { error_ = "#NUM!"; return -1; }
                    position_ += 2;
                    continue;
                }
                ++position_;
                closed = true;
                break;
            }
            if (!builder.append(source_ + position_, 1)) { error_ = "#NUM!"; return -1; }
            ++position_;
        }
        if (!closed) { error_ = "#VALUE!"; return -1; }
        Node node;
        node.kind = NodeText;
        if (!builder.finish(node.text)) { error_ = "#NUM!"; return -1; }
        return add(static_cast<Node &&>(node));
    }

    int parse_error_literal()
    {
        static const char *errors[] = { "#DIV/0!", "#VALUE!", "#NAME?", "#NUM!", "#N/A", "#CYCLE!" };
        for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
            size_t size = strlen(errors[i]);
            if (position_ + size <= length_ && !memcmp(source_ + position_, errors[i], size)) {
                Node node;
                node.kind = NodeError;
                if (!node.text.set(errors[i])) { error_ = "#NUM!"; return -1; }
                position_ += size;
                return add(static_cast<Node &&>(node));
            }
        }
        return -1;
    }

    int parse_primary()
    {
        spaces();
        if (position_ >= length_) { error_ = "#VALUE!"; return -1; }
        char c = source_[position_];
        if (c == '(') {
            if (++nesting_ > 128) { error_ = "#VALUE!"; return -1; }
            ++position_;
            int value = parse_comparison();
            spaces();
            if (value < 0 || position_ >= length_ || source_[position_] != ')') error_ = "#VALUE!";
            else ++position_;
            --nesting_;
            return error_ ? -1 : value;
        }
        if (c == '"') return parse_text();
        if (c == '#') {
            int error = parse_error_literal();
            if (error >= 0) return error;
        }
        if (ascii_digit(c) || (c == '.' && position_ + 1 < length_ && ascii_digit(source_[position_ + 1]))) {
            errno = 0;
            char *end = nullptr;
            double number = strtod(source_ + position_, &end);
            if (end == source_ + position_ || errno == ERANGE || !isfinite(number)) {
                error_ = "#NUM!";
                return -1;
            }
            position_ = static_cast<size_t>(end - source_);
            Node node;
            node.kind = NodeNumber;
            node.number = number;
            return add(static_cast<Node &&>(node));
        }

        // Function identifiers can look like out-of-grid A1-style addresses
        // (for example LOG10). Resolve the call syntax before reference syntax
        // so unsupported functions report #NAME? instead of #REF!.
        if (ascii_alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
            size_t name_end = position_;
            while (name_end < length_ && name_character(static_cast<unsigned char>(source_[name_end])))
                ++name_end;
            size_t call = name_end;
            while (call < length_ && (source_[call] == ' ' || source_[call] == '\t' ||
                                      source_[call] == '\r' || source_[call] == '\n')) ++call;
            if (call < length_ && source_[call] == '(') {
                char name[128];
                size_t size = name_end - position_;
                if (size + 1 > sizeof(name)) { error_ = "#NAME?"; return -1; }
                memcpy(name, source_ + position_, size);
                name[size] = 0;
                position_ = call;
                return parse_function(name);
            }
        }

        if (c == '[') { error_ = "#REF!"; return -1; }
        if (c == '\'' || c == '$' || c == '#' || ascii_alpha(c) ||
            static_cast<unsigned char>(c) >= 0x80) {
            size_t reference_end = position_;
            RefLex first;
            if (parse_reference(source_, length_, reference_end, sheet_, &book_, first, true)) {
                Node node;
                if (reference_end < length_ && source_[reference_end] == ':') {
                    size_t last_end = reference_end + 1;
                    RefLex last;
                    uint32_t inherited_sheet = first.qualified ? first.sheet : sheet_;
                    if (!parse_reference(source_, length_, last_end, inherited_sheet,
                                         &book_, last, true)) {
                        error_ = "#REF!";
                        return -1;
                    }
                    node.kind = NodeRange;
                    node.reference = first;
                    Node endpoint;
                    endpoint.kind = NodeReference;
                    endpoint.reference = last;
                    endpoint.next = -1;
                    int end_id = add(static_cast<Node &&>(endpoint));
                    if (end_id < 0) return -1;
                    node.left = end_id;
                    position_ = last_end;
                } else {
                    node.kind = NodeReference;
                    node.reference = first;
                    position_ = reference_end;
                }
                return add(static_cast<Node &&>(node));
            }
        }

        if (ascii_alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
            char name[128];
            size_t size = 0;
            while (position_ < length_ && name_character(static_cast<unsigned char>(source_[position_]))) {
                if (size + 1 >= sizeof(name)) { error_ = "#NAME?"; return -1; }
                name[size++] = source_[position_++];
            }
            name[size] = 0;
            spaces();
            if (position_ < length_ && source_[position_] == '(') return parse_function(name);
            if (equal_name(name, "TRUE") || equal_name(name, "FALSE")) {
                Node node;
                node.kind = NodeBoolean;
                node.number = equal_name(name, "TRUE") ? 1.0 : 0.0;
                return add(static_cast<Node &&>(node));
            }
            error_ = "#NAME?";
            return -1;
        }
        error_ = "#VALUE!";
        return -1;
    }

public:
    Parser(Workbook &book, uint32_t sheet, const char *source, Array<Node> &nodes)
        : book_(book), sheet_(sheet), source_(source), length_(strlen(source)), nodes_(nodes) {}

    int parse()
    {
        int root = parse_comparison();
        spaces();
        if (root < 0 || position_ != length_) {
            if (!error_) error_ = "#VALUE!";
            return -1;
        }
        return root;
    }
    const char *error() const { return error_ ? error_ : "#VALUE!"; }
};

struct EvalValue {
    Value value;
    bool is_range = false;
    RefLex first, last;
};

static EvalValue scalar(Value value)
{
    EvalValue result;
    result.value = static_cast<Value &&>(value);
    return result;
}

static EvalValue failure(const char *message)
{
    Value value = Value::failure(message);
    if (value.kind != ValueKind::Error) value.kind = ValueKind::Error;
    return scalar(static_cast<Value &&>(value));
}

static bool is_error(const EvalValue &value)
{
    return !value.is_range && value.value.kind == ValueKind::Error;
}

static bool parse_text_number(const char *text, double &number)
{
    if (!text) return false;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (!*text) return false;
    errno = 0;
    char *end = nullptr;
    double parsed = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(parsed)) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return false;
    number = parsed;
    return true;
}

static bool number_value(const Value &value, double &number)
{
    switch (value.kind) {
    case ValueKind::Number: number = value.number; return true;
    case ValueKind::Boolean: number = equal_name(value.text.c_str(), "TRUE") ? 1.0 : 0.0; return true;
    case ValueKind::Empty: number = 0; return true;
    case ValueKind::Text: return parse_text_number(value.text.c_str(), number);
    case ValueKind::Error: return false;
    }
    return false;
}

static bool truth_value(const Value &value, bool &result)
{
    switch (value.kind) {
    case ValueKind::Empty: result = false; return true;
    case ValueKind::Number: result = value.number != 0.0; return true;
    case ValueKind::Boolean: result = equal_name(value.text.c_str(), "TRUE"); return true;
    case ValueKind::Text:
        if (equal_name(value.text.c_str(), "FALSE") || value.text == "0" || value.text.empty())
            result = false;
        else result = true;
        return true;
    case ValueKind::Error: return false;
    }
    return false;
}

static bool append_value_text(Builder &builder, const Value &value)
{
    switch (value.kind) {
    case ValueKind::Empty: return true;
    case ValueKind::Text: return builder.append(value.text.c_str(), value.text.size());
    case ValueKind::Boolean: return builder.append(value.text.c_str());
    case ValueKind::Error: return builder.append(value.text.c_str(), value.text.size());
    case ValueKind::Number: {
        char digits[64];
        int written = snprintf(digits, sizeof(digits), "%.15g", value.number);
        return written > 0 && static_cast<size_t>(written) < sizeof(digits) &&
            builder.append(digits, static_cast<size_t>(written));
    }
    }
    return false;
}

static int compare_bytes_case_insensitive(const char *left, const char *right)
{
    while (*left && *right) {
        int a = static_cast<unsigned char>(ascii_lower(*left));
        int b = static_cast<unsigned char>(ascii_lower(*right));
        if (a != b) return a < b ? -1 : 1;
        ++left;
        ++right;
    }
    if (*left) return 1;
    if (*right) return -1;
    return 0;
}

static int compare_values(const Value &left, const Value &right)
{
    double left_number, right_number;
    bool left_numeric = number_value(left, left_number);
    bool right_numeric = number_value(right, right_number);
    if (left_numeric && right_numeric)
        return left_number < right_number ? -1 : (left_number > right_number ? 1 : 0);
    if (left.kind == ValueKind::Text && right.kind == ValueKind::Text)
        return compare_bytes_case_insensitive(left.text.c_str(), right.text.c_str());
    static const int order[] = { 0, 1, 2, 3, 4 };
    int l = order[static_cast<unsigned>(left.kind)];
    int r = order[static_cast<unsigned>(right.kind)];
    if (l != r) return l < r ? -1 : 1;
    return 0;
}

static EvalValue apply_binary(NodeKind kind, const EvalValue &left, const EvalValue &right)
{
    if (left.is_range || right.is_range) return failure("#VALUE!");
    if (left.value.kind == ValueKind::Error) return left;
    if (right.value.kind == ValueKind::Error) return right;
    if (kind == NodeConcat) {
        Builder builder;
        if (!append_value_text(builder, left.value) || !append_value_text(builder, right.value))
            return failure("#NUM!");
        Text joined;
        if (!builder.finish(joined)) return failure("#NUM!");
        Value value = Value::string(joined.c_str());
        if (value.kind != ValueKind::Text) return failure("#NUM!");
        return scalar(static_cast<Value &&>(value));
    }
    if (kind == NodeEqual || kind == NodeNotEqual || kind == NodeLess ||
        kind == NodeLessEqual || kind == NodeGreater || kind == NodeGreaterEqual) {
        int compare = compare_values(left.value, right.value);
        bool result = kind == NodeEqual ? compare == 0 :
            kind == NodeNotEqual ? compare != 0 :
            kind == NodeLess ? compare < 0 :
            kind == NodeLessEqual ? compare <= 0 :
            kind == NodeGreater ? compare > 0 : compare >= 0;
        Value value = Value::boolean(result);
        if (value.kind != ValueKind::Boolean) return failure("#NUM!");
        return scalar(static_cast<Value &&>(value));
    }
    double a, b;
    if (!number_value(left.value, a) || !number_value(right.value, b)) return failure("#VALUE!");
    double result = 0;
    switch (kind) {
    case NodeAdd: result = a + b; break;
    case NodeSubtract: result = a - b; break;
    case NodeMultiply: result = a * b; break;
    case NodeDivide:
        if (b == 0.0) return failure("#DIV/0!");
        result = a / b;
        break;
    default: return failure("#VALUE!");
    }
    if (!isfinite(result)) return failure("#NUM!");
    return scalar(Value::numeric(result));
}

static Value plain_cell_value(Cell &cell, uint64_t revision)
{
    if (cell.input.empty()) return Value{};
    if (cell.literal) {
        Value text = Value::string(cell.input.c_str());
        if (text.kind != ValueKind::Text) text.kind = ValueKind::Error;
        return text;
    }
    const char *input = cell.input.c_str();
    if (equal_name(input, "TRUE")) {
        Value value = Value::boolean(true);
        if (value.kind != ValueKind::Boolean) value.kind = ValueKind::Error;
        return value;
    }
    if (equal_name(input, "FALSE")) {
        Value value = Value::boolean(false);
        if (value.kind != ValueKind::Boolean) value.kind = ValueKind::Error;
        return value;
    }
    double number;
    if (parse_text_number(input, number)) return Value::numeric(number);
    (void)revision;
    Value text = Value::string(input);
    if (text.kind != ValueKind::Text) text.kind = ValueKind::Error;
    return text;
}

enum FrameKind : uint8_t { FrameCell, FrameExpression };

struct EvalFrame {
    FrameKind kind = FrameExpression;
    int node = -1;
    Cell *cell = nullptr;
    bool waiting = false;
    uint8_t stage = 0;
    EvalValue first, second;
    Array<EvalValue> arguments;
    int next_argument = -1;
    size_t argument_index = 0;
    int range_row = 0, range_col = 0;
    int range_last_row = -1, range_last_col = -1;
    bool range_active = false;
    double aggregate_sum = 0, aggregate_min = 0, aggregate_max = 0;
    uint64_t aggregate_count = 0;
    bool aggregate_has_value = false;
    EvalValue criteria, sum_range;
    int criteria_row = 0, criteria_col = 0;
    int criteria_last_row = -1, criteria_last_col = -1;
    int sum_row = 0, sum_col = 0;
    int sum_last_row = -1, sum_last_col = -1;
    bool criteria_is_range = false, sum_is_range = false;
    uint64_t criteria_count = 0, criteria_index = 0;
    uint64_t criteria_matches = 0;
    bool waiting_for_sum = false;
};

struct PendingInput {
    Cell *cell = nullptr;
    Text input;
};

enum RequestKind : uint8_t { RequestReady, RequestPushed };

static bool append_frame(Array<EvalFrame> &frames, EvalFrame &&frame)
{
    return frames.push(static_cast<EvalFrame &&>(frame));
}

static RequestKind request_reference(Workbook &book, const RefLex &reference,
                                     Array<Node> &nodes, Array<EvalFrame> &frames,
                                     EvalValue &ready)
{
    if (!reference.valid || !reference.sheet || reference.row < 0 ||
        reference.row >= MaxRows || reference.col < 0 || reference.col >= MaxCols) {
        ready = failure("#REF!");
        return RequestReady;
    }
    Sheet *sheet = book.sheet(reference.sheet);
    if (!sheet) {
        ready = failure("#REF!");
        return RequestReady;
    }
    Cell *cell = sheet->get(reference.row, reference.col);
    if (!cell) {
        ready = scalar(Value{});
        return RequestReady;
    }
    if (cell->evaluating) {
        ready = failure("#CYCLE!");
        return RequestReady;
    }
    if (cell->calculated == book.revision) {
        ready = scalar(cell->result);
        return RequestReady;
    }
    if (cell->literal || cell->input.empty() || cell->input.c_str()[0] != '=') {
        cell->result = plain_cell_value(*cell, book.revision);
        cell->calculated = book.revision;
        ready = scalar(cell->result);
        return RequestReady;
    }

    Parser parser(book, sheet->id, cell->input.c_str() + 1, nodes);
    int root = parser.parse();
    if (root < 0) {
        cell->result = Value::failure(parser.error());
        if (cell->result.kind != ValueKind::Error) cell->result.kind = ValueKind::Error;
        cell->calculated = book.revision;
        ready = scalar(cell->result);
        return RequestReady;
    }
    cell->evaluating = 1;
    EvalFrame cell_frame;
    cell_frame.kind = FrameCell;
    cell_frame.cell = cell;
    cell_frame.waiting = true;
    if (!append_frame(frames, static_cast<EvalFrame &&>(cell_frame))) {
        cell->evaluating = 0;
        ready = failure("#NUM!");
        return RequestReady;
    }
    EvalFrame expression;
    expression.kind = FrameExpression;
    expression.node = root;
    if (!append_frame(frames, static_cast<EvalFrame &&>(expression))) {
        frames.erase(frames.size() - 1);
        cell->evaluating = 0;
        ready = failure("#NUM!");
        return RequestReady;
    }
    return RequestPushed;
}

static bool range_bounds(const EvalValue &value, RefLex &first, RefLex &last)
{
    if (!value.is_range || !value.first.valid || !value.last.valid ||
        value.first.sheet != value.last.sheet || value.first.sheet == 0) return false;
    first = value.first;
    last = value.last;
    if (first.row > last.row) { int swap = first.row; first.row = last.row; last.row = swap; }
    if (first.col > last.col) { int swap = first.col; first.col = last.col; last.col = swap; }
    return true;
}

static bool glob_match(const char *pattern, const char *text)
{
    const char *p = pattern, *s = text;
    const char *star = nullptr, *retry = nullptr;
    while (*s) {
        if (*p == '~' && p[1]) {
            ++p;
            if (ascii_lower(*p) == ascii_lower(*s)) { ++p; ++s; continue; }
        } else if (*p == '?' || ascii_lower(*p) == ascii_lower(*s)) {
            ++p; ++s; continue;
        } else if (*p == '*') {
            star = p++;
            retry = s;
            continue;
        }
        if (star) { p = star + 1; s = ++retry; continue; }
        return false;
    }
    while (*p == '*') ++p;
    return !*p;
}

struct Criterion {
    enum Operation : uint8_t { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual } operation = Equal;
    Value target;
    bool wildcard = false;
};

static bool make_criterion(const Value &value, Criterion &criterion)
{
    if (value.kind == ValueKind::Error) return false;
    if (value.kind == ValueKind::Number || value.kind == ValueKind::Boolean || value.kind == ValueKind::Empty) {
        criterion.target = value;
        return true;
    }
    const char *text = value.text.c_str();
    const char *body = text;
    if (!strncmp(text, ">=", 2)) { criterion.operation = Criterion::GreaterEqual; body += 2; }
    else if (!strncmp(text, "<=", 2)) { criterion.operation = Criterion::LessEqual; body += 2; }
    else if (!strncmp(text, "<>", 2)) { criterion.operation = Criterion::NotEqual; body += 2; }
    else if (*text == '>') { criterion.operation = Criterion::Greater; ++body; }
    else if (*text == '<') { criterion.operation = Criterion::Less; ++body; }
    else if (*text == '=') { criterion.operation = Criterion::Equal; ++body; }
    double number;
    if (parse_text_number(body, number)) criterion.target = Value::numeric(number);
    else {
        criterion.target = Value::string(body);
        if (criterion.target.kind != ValueKind::Text) return false;
    }
    for (const char *p = body; *p; ++p)
        if (*p == '*' || *p == '?' || *p == '~') criterion.wildcard = true;
    return true;
}

static bool criterion_matches(const Value &candidate, const Criterion &criterion)
{
    if (candidate.kind == ValueKind::Error) return false;
    if (criterion.target.kind == ValueKind::Number && candidate.kind == ValueKind::Text) {
        double parsed;
        if (!parse_text_number(candidate.text.c_str(), parsed)) return false;
        Value numeric_candidate = Value::numeric(parsed);
        int numeric_compare = compare_values(numeric_candidate, criterion.target);
        switch (criterion.operation) {
        case Criterion::Equal: return numeric_compare == 0;
        case Criterion::NotEqual: return numeric_compare != 0;
        case Criterion::Less: return numeric_compare < 0;
        case Criterion::LessEqual: return numeric_compare <= 0;
        case Criterion::Greater: return numeric_compare > 0;
        case Criterion::GreaterEqual: return numeric_compare >= 0;
        }
    }
    int comparison;
    if (criterion.wildcard && candidate.kind == ValueKind::Text &&
        criterion.operation == Criterion::Equal)
        return glob_match(criterion.target.text.c_str(), candidate.text.c_str());
    comparison = compare_values(candidate, criterion.target);
    switch (criterion.operation) {
    case Criterion::Equal: return comparison == 0;
    case Criterion::NotEqual: return comparison != 0;
    case Criterion::Less: return comparison < 0;
    case Criterion::LessEqual: return comparison <= 0;
    case Criterion::Greater: return comparison > 0;
    case Criterion::GreaterEqual: return comparison >= 0;
    }
    return false;
}

static bool aggregate_feed(EvalFrame &frame, FunctionKind function,
                           const Value &value, bool from_range, EvalValue &error)
{
    if (function == FunctionCountA) {
        if (value.kind != ValueKind::Empty) ++frame.aggregate_count;
        return true;
    }
    if (function == FunctionCount) {
        if (value.kind == ValueKind::Number) ++frame.aggregate_count;
        return true;
    }
    if (value.kind == ValueKind::Error) { error = scalar(value); return false; }
    if (value.kind == ValueKind::Empty || value.kind == ValueKind::Text) return true;
    double number;
    if (value.kind == ValueKind::Boolean) {
        if (from_range) return true;
        number = value.number != 0 ? 1.0 : 0.0;
    } else if (value.kind == ValueKind::Number) number = value.number;
    else return true;
    frame.aggregate_sum += number;
    if (!frame.aggregate_has_value) {
        frame.aggregate_min = frame.aggregate_max = number;
        frame.aggregate_has_value = true;
    } else {
        if (number < frame.aggregate_min) frame.aggregate_min = number;
        if (number > frame.aggregate_max) frame.aggregate_max = number;
    }
    ++frame.aggregate_count;
    if (!isfinite(frame.aggregate_sum)) { error = failure("#NUM!"); return false; }
    return true;
}

static EvalValue finish_aggregate(const EvalFrame &frame, FunctionKind function)
{
    switch (function) {
    case FunctionSum: return scalar(Value::numeric(frame.aggregate_sum));
    case FunctionAverage:
        if (!frame.aggregate_count) return failure("#DIV/0!");
        return scalar(Value::numeric(frame.aggregate_sum / static_cast<double>(frame.aggregate_count)));
    case FunctionMin: return scalar(Value::numeric(frame.aggregate_has_value ? frame.aggregate_min : 0));
    case FunctionMax: return scalar(Value::numeric(frame.aggregate_has_value ? frame.aggregate_max : 0));
    case FunctionCount: case FunctionCountA:
        return scalar(Value::numeric(static_cast<double>(frame.aggregate_count)));
    default: return failure("#VALUE!");
    }
}

static bool next_range_position(int &row, int &col, int first_col,
                                int last_row, int last_col)
{
    if (col < last_col) { ++col; return true; }
    if (row < last_row) { ++row; col = first_col; return true; }
    return false;
}

static EvalValue unary_result(NodeKind kind, const EvalValue &child)
{
    if (child.is_range) return failure("#VALUE!");
    if (child.value.kind == ValueKind::Error) return child;
    double number;
    if (!number_value(child.value, number)) return failure("#VALUE!");
    if (kind == NodeUnaryMinus) number = -number;
    return isfinite(number) ? scalar(Value::numeric(number)) : failure("#NUM!");
}

static EvalValue call_scalar_function(FunctionKind function, const Array<EvalValue> &arguments)
{
    if (function == FunctionAbs || function == FunctionRound) {
        const EvalValue &first = arguments[0];
        if (first.is_range) return failure("#VALUE!");
        if (first.value.kind == ValueKind::Error) return first;
        double number;
        if (!number_value(first.value, number)) return failure("#VALUE!");
        if (function == FunctionAbs) return scalar(Value::numeric(fabs(number)));
        if (arguments[1].is_range) return failure("#VALUE!");
        if (arguments[1].value.kind == ValueKind::Error) return arguments[1];
        double digits_value;
        if (!number_value(arguments[1].value, digits_value) || digits_value < -308 || digits_value > 308)
            return failure("#NUM!");
        int digits = static_cast<int>(digits_value);
        double scale = pow(10.0, static_cast<double>(digits));
        if (!isfinite(scale) || scale == 0.0) return failure("#NUM!");
        double scaled = number * scale;
        if (!isfinite(scaled)) return failure("#NUM!");
        double rounded = (scaled < 0 ? ceil(scaled - 0.5) : floor(scaled + 0.5)) / scale;
        return isfinite(rounded) ? scalar(Value::numeric(rounded)) : failure("#NUM!");
    }
    return failure("#VALUE!");
}

static bool valid_function_arity(FunctionKind function, unsigned count)
{
    switch (function) {
    case FunctionIf: return count == 2 || count == 3;
    case FunctionRound: return count == 2;
    case FunctionAbs: return count == 1;
    case FunctionCountIf: return count == 2;
    case FunctionSumIf: return count == 2 || count == 3;
    case FunctionUnknown: return true;
    default: return true;
    }
}

static bool begin_range_iteration(EvalValue &value, int &row, int &col,
                                  int &last_row, int &last_col)
{
    RefLex first, last;
    if (!range_bounds(value, first, last)) return false;
    row = first.row;
    col = first.col;
    last_row = last.row;
    last_col = last.col;
    return true;
}

static void clean_evaluating(Array<EvalFrame> &frames)
{
    for (size_t i = 0; i < frames.size(); ++i)
        if (frames[i].kind == FrameCell && frames[i].cell) frames[i].cell->evaluating = 0;
}

static bool push_expression(Array<EvalFrame> &frames, int node)
{
    EvalFrame frame;
    frame.kind = FrameExpression;
    frame.node = node;
    return append_frame(frames, static_cast<EvalFrame &&>(frame));
}

static bool push_function_argument(EvalFrame &frame, Array<EvalFrame> &frames,
                                   int node, uint8_t wait_stage)
{
    frame.stage = wait_stage;
    frame.waiting = true;
    return push_expression(frames, node);
}

static EvalValue evaluate_root(Workbook &book, uint32_t sheet_id, int row, int col)
{
    Array<Node> nodes;
    Array<EvalFrame> frames;
    RefLex reference;
    reference.sheet = sheet_id;
    reference.row = row;
    reference.col = col;
    reference.valid = row >= 0 && row < MaxRows && col >= 0 && col < MaxCols && book.sheet(sheet_id);
    EvalValue immediate;
    RequestKind request = request_reference(book, reference, nodes, frames, immediate);
    if (request == RequestReady) return immediate;

    EvalValue returned;
    bool has_returned = false;
    while (!frames.empty()) {
        size_t top = frames.size() - 1;
        if (frames[top].waiting) {
            if (!has_returned) {
                clean_evaluating(frames);
                return failure("#VALUE!");
            }
            EvalValue child = static_cast<EvalValue &&>(returned);
            has_returned = false;
            frames[top].waiting = false;
            EvalFrame &waiting = frames[top];
            if (waiting.kind == FrameCell) {
                waiting.cell->result = child.value;
                waiting.cell->calculated = book.revision;
                waiting.cell->evaluating = 0;
                frames.erase(top);
                returned = static_cast<EvalValue &&>(child);
                has_returned = true;
                continue;
            }
            switch (waiting.stage) {
            case 1: waiting.first = static_cast<EvalValue &&>(child); break;
            case 2: waiting.second = static_cast<EvalValue &&>(child); break;
            case 3: waiting.first = static_cast<EvalValue &&>(child); break;
            case 10:
                if (!waiting.arguments.push(static_cast<EvalValue &&>(child))) {
                    returned = failure("#NUM!");
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                break;
            case 21:
                waiting.first = static_cast<EvalValue &&>(child);
                break;
            case 31:
                waiting.first = static_cast<EvalValue &&>(child);
                break;
            case 32:
                // Keep the running SUMIF total in second while the sum cell
                // arrives separately in first.
                waiting.first = static_cast<EvalValue &&>(child);
                break;
            case 40:
                waiting.first = static_cast<EvalValue &&>(child);
                break;
            default:
                returned = failure("#VALUE!");
                frames.erase(top);
                has_returned = true;
                continue;
            }
        }

        EvalFrame &frame = frames[top];
        if (frame.kind == FrameCell) {
            returned = failure("#VALUE!");
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (frame.node < 0 || static_cast<size_t>(frame.node) >= nodes.size()) {
            returned = failure("#VALUE!");
            frames.erase(top);
            has_returned = true;
            continue;
        }
        const Node &node = nodes[static_cast<size_t>(frame.node)];

        if (node.kind == NodeNumber) {
            returned = scalar(Value::numeric(node.number));
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeText) {
            Value value = Value::string(node.text.c_str());
            returned = value.kind == ValueKind::Text ? scalar(static_cast<Value &&>(value)) : failure("#NUM!");
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeBoolean) {
            Value value = Value::boolean(node.number != 0);
            returned = value.kind == ValueKind::Boolean ? scalar(static_cast<Value &&>(value)) : failure("#NUM!");
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeError) {
            returned = failure(node.text.c_str());
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeReference) {
            if (frame.stage == 40) {
                returned = static_cast<EvalValue &&>(frame.first);
                frames.erase(top);
                has_returned = true;
                continue;
            }
            EvalValue value;
            frame.waiting = true;
            frame.stage = 40;
            if (request_reference(book, node.reference, nodes, frames, value) == RequestPushed) {
                continue;
            }
            frame.waiting = false;
            frame.stage = 0;
            returned = static_cast<EvalValue &&>(value);
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeRange) {
            EvalValue value;
            value.is_range = true;
            value.first = node.reference;
            if (node.left < 0 || static_cast<size_t>(node.left) >= nodes.size()) {
                returned = failure("#REF!");
            } else {
                value.last = nodes[static_cast<size_t>(node.left)].reference;
                RefLex first, last;
                if (!range_bounds(value, first, last)) returned = failure("#REF!");
                else returned = static_cast<EvalValue &&>(value);
            }
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeUnaryPlus || node.kind == NodeUnaryMinus) {
            if (frame.stage == 0) {
                if (node.left < 0 || !push_function_argument(frame, frames, node.left, 1)) {
                    returned = failure("#NUM!");
                    frames.erase(top);
                    has_returned = true;
                }
                continue;
            }
            returned = unary_result(node.kind, frame.first);
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind >= NodeAdd && node.kind <= NodeGreaterEqual) {
            if (frame.stage == 0) {
                if (node.left < 0 || !push_function_argument(frame, frames, node.left, 1)) {
                    returned = failure("#NUM!");
                    frames.erase(top);
                    has_returned = true;
                }
                continue;
            }
            if (frame.stage == 1) {
                if (is_error(frame.first)) {
                    returned = static_cast<EvalValue &&>(frame.first);
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                if (node.right < 0 || !push_function_argument(frame, frames, node.right, 2)) {
                    returned = failure("#NUM!");
                    frames.erase(top);
                    has_returned = true;
                }
                continue;
            }
            returned = apply_binary(node.kind, frame.first, frame.second);
            frames.erase(top);
            has_returned = true;
            continue;
        }
        if (node.kind == NodeFunction) {
            if (node.function == FunctionUnknown) {
                returned = failure("#NAME?");
                frames.erase(top);
                has_returned = true;
                continue;
            }
            if (!valid_function_arity(node.function, node.argument_count)) {
                returned = failure("#VALUE!");
                frames.erase(top);
                has_returned = true;
                continue;
            }
            if (node.function == FunctionIf) {
                if (frame.stage == 0) {
                    int first_arg = node.first_argument;
                    if (first_arg < 0 || !push_function_argument(frame, frames, first_arg, 1)) {
                        returned = failure("#NUM!");
                        frames.erase(top);
                        has_returned = true;
                    }
                    continue;
                }
                if (frame.stage == 1) {
                    if (is_error(frame.first)) {
                        returned = static_cast<EvalValue &&>(frame.first);
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    bool condition = false;
                    if (frame.first.is_range || !truth_value(frame.first.value, condition)) {
                        returned = failure("#VALUE!");
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    int branch = nodes[static_cast<size_t>(node.first_argument)].next;
                    if (!condition && node.argument_count == 3)
                        branch = nodes[static_cast<size_t>(branch)].next;
                    if (!condition && node.argument_count == 2) {
                        returned = scalar(Value::numeric(0));
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    if (branch < 0 || !push_function_argument(frame, frames, branch, 3)) {
                        returned = failure("#NUM!");
                        frames.erase(top);
                        has_returned = true;
                    }
                    continue;
                }
                returned = static_cast<EvalValue &&>(frame.first);
                frames.erase(top);
                has_returned = true;
                continue;
            }
            if (frame.stage == 0) {
                frame.next_argument = node.first_argument;
                frame.stage = 10;
                if (frame.next_argument >= 0) {
                    int argument = frame.next_argument;
                    frame.next_argument = nodes[static_cast<size_t>(argument)].next;
                    frame.waiting = true;
                    if (!push_expression(frames, argument)) {
                        frame.waiting = false;
                        returned = failure("#NUM!");
                        frames.erase(top);
                        has_returned = true;
                    }
                } else {
                    if (node.function == FunctionAbs || node.function == FunctionRound) {
                        returned = failure("#VALUE!");
                        frames.erase(top);
                        has_returned = true;
                    } else {
                        frame.stage = 20;
                    }
                }
                continue;
            }
            if (frame.stage == 10) {
                if (frame.next_argument >= 0) {
                    int argument = frame.next_argument;
                    frame.next_argument = nodes[static_cast<size_t>(argument)].next;
                    frame.waiting = true;
                    if (!push_expression(frames, argument)) {
                        frame.waiting = false;
                        returned = failure("#NUM!");
                        frames.erase(top);
                        has_returned = true;
                    }
                    continue;
                }
                if (node.function == FunctionAbs || node.function == FunctionRound) {
                    returned = call_scalar_function(node.function, frame.arguments);
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                if (node.function == FunctionCountIf || node.function == FunctionSumIf) {
                    if (frame.arguments.size() < 2 || frame.arguments.size() > 3) {
                        returned = failure("#VALUE!");
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    frame.criteria = frame.arguments[1];
                    frame.sum_range = node.function == FunctionSumIf ?
                        (frame.arguments.size() == 3 ? frame.arguments[2] : frame.arguments[0]) :
                        EvalValue{};
                    if (frame.criteria.is_range) {
                        returned = failure("#VALUE!");
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    if (frame.criteria.value.kind == ValueKind::Error) {
                        returned = frame.criteria;
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    if (frame.arguments[0].is_range) {
                        if (!begin_range_iteration(frame.arguments[0], frame.criteria_row,
                                                   frame.criteria_col, frame.criteria_last_row,
                                                   frame.criteria_last_col)) {
                            returned = failure("#REF!");
                            frames.erase(top);
                            has_returned = true;
                            continue;
                        }
                        frame.criteria_is_range = true;
                        frame.criteria_count = static_cast<uint64_t>(frame.criteria_last_row - frame.criteria_row + 1) *
                            static_cast<uint64_t>(frame.criteria_last_col - frame.criteria_col + 1);
                    } else {
                        frame.criteria_is_range = false;
                        frame.criteria_count = 1;
                    }
                    if (node.function == FunctionSumIf) {
                        if (frame.sum_range.is_range) {
                            if (!begin_range_iteration(frame.sum_range, frame.sum_row, frame.sum_col,
                                                       frame.sum_last_row, frame.sum_last_col)) {
                                returned = failure("#REF!");
                                frames.erase(top);
                                has_returned = true;
                                continue;
                            }
                            frame.sum_is_range = true;
                            bool same_shape = frame.criteria_is_range &&
                                frame.sum_last_row - frame.sum_row ==
                                    frame.criteria_last_row - frame.criteria_row &&
                                frame.sum_last_col - frame.sum_col ==
                                    frame.criteria_last_col - frame.criteria_col;
                            if (!same_shape) {
                                returned = failure("#VALUE!");
                                frames.erase(top);
                                has_returned = true;
                                continue;
                            }
                        } else if (frame.criteria_is_range) {
                            returned = failure("#VALUE!");
                            frames.erase(top);
                            has_returned = true;
                            continue;
                        }
                    }
                    Criterion criterion;
                    if (!make_criterion(frame.criteria.value, criterion)) {
                        returned = failure("#VALUE!");
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    frame.stage = 30;
                    frame.argument_index = 0;
                    frame.second = scalar(Value::numeric(0));
                    continue;
                }
                frame.stage = 20;
                frame.argument_index = 0;
                continue;
            }
            if (frame.stage == 20) {
                if (frame.argument_index >= frame.arguments.size()) {
                    returned = finish_aggregate(frame, node.function);
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                EvalValue &argument = frame.arguments[frame.argument_index];
                if (argument.is_range) {
                    if (!frame.range_active) {
                        if (!begin_range_iteration(argument, frame.range_row, frame.range_col,
                                                   frame.range_last_row, frame.range_last_col)) {
                            returned = failure("#REF!");
                            frames.erase(top);
                            has_returned = true;
                            continue;
                        }
                        frame.range_active = true;
                    }
                    RefLex reference = argument.first;
                    reference.row = frame.range_row;
                    reference.col = frame.range_col;
                    if (!next_range_position(frame.range_row, frame.range_col,
                                             argument.first.col < argument.last.col ?
                                             argument.first.col : argument.last.col,
                                             frame.range_last_row, frame.range_last_col)) {
                        frame.range_active = false;
                        ++frame.argument_index;
                    }
                    EvalValue child;
                    frame.waiting = true;
                    frame.stage = 21;
                    if (request_reference(book, reference, nodes, frames, child) == RequestPushed) {
                        continue;
                    }
                    frame.waiting = false;
                    frame.stage = 20;
                    EvalValue aggregate_error;
                    if (!aggregate_feed(frame, node.function, child.value, true, aggregate_error)) {
                        returned = static_cast<EvalValue &&>(aggregate_error);
                        frames.erase(top);
                        has_returned = true;
                    }
                    continue;
                }
                EvalValue aggregate_error;
                if (!aggregate_feed(frame, node.function, argument.value, false, aggregate_error)) {
                    returned = static_cast<EvalValue &&>(aggregate_error);
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                ++frame.argument_index;
                continue;
            }
            if (frame.stage == 21) {
                EvalValue aggregate_error;
                if (!aggregate_feed(frame, node.function, frame.first.value, true, aggregate_error)) {
                    returned = static_cast<EvalValue &&>(aggregate_error);
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                frame.stage = 20;
                continue;
            }
            if (frame.stage == 30) {
                if (frame.argument_index >= frame.criteria_count) {
                    returned = node.function == FunctionCountIf ?
                        scalar(Value::numeric(static_cast<double>(frame.criteria_matches))) :
                        static_cast<EvalValue &&>(frame.second);
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                int criteria_row = frame.criteria_row;
                int criteria_col = frame.criteria_col;
                if (frame.criteria_is_range) {
                    // Coordinates advance in row-major order using the normalized top-left range.
                    int start_col = frame.arguments[0].first.col < frame.arguments[0].last.col ?
                        frame.arguments[0].first.col : frame.arguments[0].last.col;
                    int start_row = frame.arguments[0].first.row < frame.arguments[0].last.row ?
                        frame.arguments[0].first.row : frame.arguments[0].last.row;
                    uint64_t columns = static_cast<uint64_t>(frame.criteria_last_col - start_col + 1);
                    uint64_t index = frame.argument_index;
                    criteria_row = start_row + static_cast<int>(index / columns);
                    criteria_col = start_col + static_cast<int>(index % columns);
                }
                if (node.function == FunctionSumIf && frame.waiting_for_sum) {
                    returned = failure("#VALUE!");
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                if (node.function == FunctionSumIf && frame.waiting_for_sum) {
                    frame.waiting_for_sum = false;
                }
                RefLex criteria_ref;
                EvalValue criteria_value;
                if (frame.criteria_is_range) {
                    criteria_ref = frame.arguments[0].first;
                    criteria_ref.row = criteria_row;
                    criteria_ref.col = criteria_col;
                    frame.waiting = true;
                    frame.stage = 31;
                    if (request_reference(book, criteria_ref, nodes, frames, criteria_value) == RequestPushed) {
                        continue;
                    }
                    frame.waiting = false;
                    frame.stage = 30;
                } else criteria_value = frame.arguments[0];
                Criterion criterion;
                if (!make_criterion(frame.criteria.value, criterion)) {
                    returned = failure("#VALUE!");
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                if (criterion_matches(criteria_value.value, criterion)) {
                    ++frame.criteria_matches;
                    if (node.function == FunctionSumIf) {
                        if (frame.sum_is_range) {
                            int start_col = frame.sum_range.first.col < frame.sum_range.last.col ?
                                frame.sum_range.first.col : frame.sum_range.last.col;
                            int start_row = frame.sum_range.first.row < frame.sum_range.last.row ?
                                frame.sum_range.first.row : frame.sum_range.last.row;
                            uint64_t columns = static_cast<uint64_t>(frame.sum_last_col - start_col + 1);
                            frame.sum_row = start_row + static_cast<int>(frame.argument_index / columns);
                            frame.sum_col = start_col + static_cast<int>(frame.argument_index % columns);
                            RefLex sum_ref = frame.sum_range.first;
                            sum_ref.row = frame.sum_row;
                            sum_ref.col = frame.sum_col;
                            frame.waiting = true;
                            frame.stage = 32;
                            if (request_reference(book, sum_ref, nodes, frames, criteria_value) == RequestPushed) {
                                continue;
                            }
                            frame.waiting = false;
                            frame.stage = 30;
                        } else criteria_value = frame.sum_range;
                        if (criteria_value.value.kind == ValueKind::Error) {
                            returned = criteria_value;
                            frames.erase(top);
                            has_returned = true;
                            continue;
                        }
                        double amount;
                        if (number_value(criteria_value.value, amount)) {
                            double sum = frame.second.value.number + amount;
                            if (!isfinite(sum)) {
                                returned = failure("#NUM!");
                                frames.erase(top);
                                has_returned = true;
                                continue;
                            }
                            frame.second = scalar(Value::numeric(sum));
                        }
                    }
                }
                ++frame.argument_index;
                continue;
            }
            if (frame.stage == 31) {
                // The criteria cell was delivered in first; retry this pair using that value.
                frame.stage = 30;
                continue;
            }
            if (frame.stage == 32) {
                // The matching sum cell arrived in first; add it and finish this pair.
                if (frame.first.value.kind == ValueKind::Error) {
                    returned = frame.first;
                    frames.erase(top);
                    has_returned = true;
                    continue;
                }
                double amount;
                if (number_value(frame.first.value, amount)) {
                    double sum = frame.second.value.number + amount;
                    if (!isfinite(sum)) {
                        returned = failure("#NUM!");
                        frames.erase(top);
                        has_returned = true;
                        continue;
                    }
                    frame.second = scalar(Value::numeric(sum));
                }
                ++frame.argument_index;
                frame.stage = 30;
                continue;
            }
        }
        returned = failure("#VALUE!");
        frames.erase(top);
        has_returned = true;
    }
    if (!has_returned) return scalar(Value{});
    return returned;
}


} // namespace

Value evaluate(Workbook &book, uint32_t sheet, int row, int column)
{
    EvalValue result = evaluate_root(book, sheet, row, column);
    if (result.is_range) return Value::failure("#VALUE!");
    return result.value;
}

bool recalculate(Workbook &book, Progress progress, void *context)
{
    unsigned total = 0;
    for (size_t sheet_index = 0; sheet_index < book.sheets.size(); ++sheet_index) {
        const Sheet &sheet = book.sheets[sheet_index];
        for (size_t cell_index = 0; cell_index < sheet.cells.size(); ++cell_index) {
            const Cell &cell = sheet.cells[cell_index];
            if (!cell.literal && !cell.input.empty() && cell.input.c_str()[0] == '=') ++total;
        }
    }
    unsigned completed = 0;
    for (size_t sheet_index = 0; sheet_index < book.sheets.size(); ++sheet_index) {
        const Sheet &sheet = book.sheets[sheet_index];
        for (size_t cell_index = 0; cell_index < sheet.cells.size(); ++cell_index) {
            const Cell &cell = sheet.cells[cell_index];
            if (cell.literal || cell.input.empty() || cell.input.c_str()[0] != '=') continue;
            (void)evaluate(book, sheet.id, cell.row, cell.col);
            ++completed;
            if (progress && !progress(context, completed, total)) return false;
        }
    }
    return true;
}

bool translate_formula(Text &output, const char *input, int row_delta, int column_delta)
{
    if (!input) return false;
    size_t length = strlen(input);
    Builder builder;
    size_t cursor = 0, copied = 0;
    while (cursor < length) {
        if (input[cursor] == '"') {
            size_t end = cursor + 1;
            while (end < length) {
                if (input[end] == '"') {
                    if (end + 1 < length && input[end + 1] == '"') { end += 2; continue; }
                    ++end;
                    break;
                }
                ++end;
            }
            cursor = end;
            continue;
        }
        if (cursor && (name_character(static_cast<unsigned char>(input[cursor - 1])) ||
                       input[cursor - 1] == '!' || input[cursor - 1] == ']' ||
                       input[cursor - 1] == '[' || input[cursor - 1] == '$')) {
            ++cursor;
            continue;
        }
        RefGroup group;
        if (parse_reference_group(input, length, cursor, 0, nullptr, group)) {
            if (reference_is_function_name(input, length, group)) {
                cursor = group.end;
                continue;
            }
            if (!builder.append(input + copied, cursor - copied)) return false;
            int row1, col1, row2, col2;
            bool invalid1, invalid2;
            translate_one(group.first, row_delta, column_delta, row1, col1, invalid1);
            if (group.range) {
                translate_one(group.last, row_delta, column_delta, row2, col2, invalid2);
                if (!append_original_reference(builder, input, group.first,
                                               row1, col1, invalid1)) return false;
                if (!builder.append(":")) return false;
                if (!append_original_reference(builder, input, group.last,
                                               row2, col2, invalid2)) return false;
            } else if (!append_original_reference(builder, input, group.first,
                                                  row1, col1, invalid1)) return false;
            cursor = group.end;
            copied = cursor;
            continue;
        }
        ++cursor;
    }
    if (!builder.append(input + copied, length - copied)) return false;
    return builder.finish(output);
}

bool rewrite_axis(Workbook &book, uint32_t sheet_id, bool rows, int first,
                  int count, bool deleted)
{
    if (!book.sheet(sheet_id) || first < 0 || count <= 0 ||
        first >= (rows ? MaxRows : MaxCols) ||
        count > (rows ? MaxRows : MaxCols) - first) return false;
    Array<PendingInput> pending;
    for (size_t sheet_index = 0; sheet_index < book.sheets.size(); ++sheet_index) {
        Sheet &sheet = book.sheets[sheet_index];
        for (size_t cell_index = 0; cell_index < sheet.cells.size(); ++cell_index) {
            Cell &cell = sheet.cells[cell_index];
            if (cell.literal || cell.input.empty() || cell.input.c_str()[0] != '=') continue;
            Text rewritten;
            if (!rewrite_formula_axis(rewritten, cell.input.c_str(), sheet.id, sheet_id,
                                      rows, first, count, deleted, book)) return false;
            if (!(rewritten == cell.input)) {
                PendingInput update;
                update.cell = &cell;
                update.input = static_cast<Text &&>(rewritten);
                if (!pending.push(static_cast<PendingInput &&>(update))) return false;
            }
        }
    }
    for (size_t i = 0; i < pending.size(); ++i)
        pending[i].cell->input = static_cast<Text &&>(pending[i].input);
    book.invalidate();
    return true;
}

bool rewrite_sheet_name(Workbook &book, const char *old_name, const char *new_name)
{
    if (!old_name || !*old_name || (new_name && !*new_name)) return false;
    Array<PendingInput> pending;
    for (size_t sheet_index = 0; sheet_index < book.sheets.size(); ++sheet_index) {
        Sheet &sheet = book.sheets[sheet_index];
        for (size_t cell_index = 0; cell_index < sheet.cells.size(); ++cell_index) {
            Cell &cell = sheet.cells[cell_index];
            if (cell.literal || cell.input.empty() || cell.input.c_str()[0] != '=') continue;
            Text rewritten;
            if (!rewrite_formula_sheet_name(rewritten, cell.input.c_str(), old_name, new_name))
                return false;
            if (!(rewritten == cell.input)) {
                PendingInput update;
                update.cell = &cell;
                update.input = static_cast<Text &&>(rewritten);
                if (!pending.push(static_cast<PendingInput &&>(update))) return false;
            }
        }
    }
    for (size_t i = 0; i < pending.size(); ++i)
        pending[i].cell->input = static_cast<Text &&>(pending[i].input);
    book.invalidate();
    return true;
}

bool rewrite_move_references(Workbook &book, uint32_t source_sheet_id, Range source,
                             uint32_t target_sheet_id, int target_row, int target_col,
                             bool &unsupported, Progress progress, void *context)
{
    unsupported = false;
    const Sheet *source_sheet = book.sheet(source_sheet_id);
    const Sheet *target_sheet = book.sheet(target_sheet_id);
    const int height = source.r2 - source.r1 + 1;
    const int width = source.c2 - source.c1 + 1;
    if (!source_sheet || !target_sheet || !source.valid() || target_row < 0 || target_col < 0 ||
        target_row > MaxRows - height || target_col > MaxCols - width) return false;
    if (source_sheet_id == target_sheet_id && target_row == source.r1 && target_col == source.c1) return true;

    Range destination{target_row, target_col, target_row + height - 1, target_col + width - 1};
    unsigned total = 0;
    for (size_t sheet_index = 0; sheet_index < book.sheets.size(); ++sheet_index) {
        const Sheet &sheet = book.sheets[sheet_index];
        for (size_t cell_index = 0; cell_index < sheet.cells.size(); ++cell_index) {
            const Cell &cell = sheet.cells[cell_index];
            bool moving_formula = sheet.id == source_sheet_id && source.contains(cell.row, cell.col);
            bool overwritten = sheet.id == target_sheet_id && destination.contains(cell.row, cell.col) && !moving_formula;
            if (overwritten || cell.literal || cell.input.empty() || cell.input.c_str()[0] != '=') continue;
            ++total;
        }
    }
    if (progress && !progress(context, 0, total)) return false;

    Array<PendingInput> pending;
    unsigned completed = 0;
    for (size_t sheet_index = 0; sheet_index < book.sheets.size(); ++sheet_index) {
        Sheet &sheet = book.sheets[sheet_index];
        for (size_t cell_index = 0; cell_index < sheet.cells.size(); ++cell_index) {
            Cell &cell = sheet.cells[cell_index];
            bool moving_formula = sheet.id == source_sheet_id && source.contains(cell.row, cell.col);
            bool overwritten = sheet.id == target_sheet_id && destination.contains(cell.row, cell.col) && !moving_formula;
            if (overwritten || cell.literal || cell.input.empty() || cell.input.c_str()[0] != '=') continue;
            Text rewritten;
            bool changed = false;
            if (!rewrite_formula_move(rewritten, cell.input.c_str(), sheet.id, moving_formula,
                                      source_sheet_id, source, target_sheet_id, target_row, target_col,
                                      book, changed, unsupported)) return false;
            if (unsupported) return true;
            if (changed && !(rewritten == cell.input)) {
                PendingInput update;
                update.cell = &cell;
                update.input = static_cast<Text &&>(rewritten);
                if (!pending.push(static_cast<PendingInput &&>(update))) return false;
            }
            if ((++completed & 255u) == 0 && progress && !progress(context, completed, total)) return false;
        }
    }
    if (progress && !progress(context, completed, total)) return false;
    for (size_t i = 0; i < pending.size(); ++i)
        pending[i].cell->input = static_cast<Text &&>(pending[i].input);
    if (!pending.empty()) book.invalidate();
    return true;
}

} // namespace rx
