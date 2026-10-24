#include "editor_core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace rm {

struct EditorUndoState {
    EditorUndoState *previous;
    char *source;
    size_t source_size;
    Range *stops;
    size_t stop_count;
    size_t active_stop;
};

namespace {

const size_t kNoStop = static_cast<size_t>(-1);
const size_t kUndoLimit = 64;

bool failure(Error &error, Code code, const char *message,
             size_t begin = 0, size_t end = 0) {
    error.set(code, message, begin, end);
    return false;
}

bool source_length(const char *source, size_t &length, Error &error) {
    if (!source) return failure(error, Code::Format, "Source text is null");
    length = 0;
    while (length < kMaxSourceBytes && source[length]) ++length;
    if (length == kMaxSourceBytes && source[length]) {
        return failure(error, Code::Limit, "Text exceeds the source byte limit");
    }
    return true;
}

bool valid_utf8(const char *bytes, size_t size, Error &error) {
    size_t bad_offset = 0;
    if (!detail::valid_utf8(bytes, size, &bad_offset)) {
        return failure(error, Code::InvalidUtf8, "Text is not valid UTF-8",
                       bad_offset, bad_offset + 1);
    }
    return true;
}

bool is_boundary(const char *bytes, size_t size, size_t offset) {
    if (offset > size) return false;
    if (offset == 0 || offset == size) return true;
    return (static_cast<uint8_t>(bytes[offset]) & 0xc0u) != 0x80u;
}

bool valid_range(const char *source, size_t size, Range range, Error &error) {
    if (range.begin > range.end || range.end > size) {
        return failure(error, Code::Format, "Text range is outside the source",
                       range.begin, range.end);
    }
    if (!is_boundary(source, size, range.begin) ||
        !is_boundary(source, size, range.end)) {
        return failure(error, Code::Format,
                       "Text range must start and end at UTF-8 boundaries",
                       range.begin, range.end);
    }
    return true;
}

bool append_bytes(char *destination, size_t capacity, size_t &offset,
                  const char *source, size_t length) {
    if (length > capacity - offset) return false;
    if (length) memcpy(destination + offset, source, length);
    offset += length;
    return true;
}

bool make_replaced_text(const char *source, size_t source_size, Range range,
                        const char *replacement, size_t replacement_size, Text &out_text,
                        Error &error) {
    if (!valid_range(source, source_size, range, error)) return false;
    const size_t removed = range.end - range.begin;
    const size_t kept_size = source_size - removed;
    if (kept_size > kMaxSourceBytes || replacement_size > kMaxSourceBytes - kept_size) {
        return failure(error, Code::Limit, "Replaced text is too large");
    }
    if (!valid_utf8(replacement, replacement_size, error)) return false;
    const size_t result_size = kept_size + replacement_size;
    char *result = static_cast<char *>(malloc(result_size + 1));
    if (!result) return failure(error, Code::NoMemory, "Unable to allocate replaced text");
    size_t written = 0;
    const bool assembled = append_bytes(result, result_size, written, source,
                                        range.begin) &&
                           append_bytes(result, result_size, written, replacement,
                                        replacement_size) &&
                           append_bytes(result, result_size, written,
                                        source + range.end,
                                        source_size - range.end);
    if (!assembled || written != result_size) {
        free(result);
        return failure(error, Code::Format, "Unable to assemble replaced text");
    }
    result[result_size] = '\0';
    const bool stored = out_text.set(result, result_size);
    free(result);
    if (!stored) {
        const Code code = result_size > kMaxTextBytes ? Code::Limit : Code::NoMemory;
        return failure(error, code, "Unable to store replaced text");
    }
    return true;
}

bool save_undo(EditorCore &editor, Error &error) {
    if (editor.source.size() == SIZE_MAX ||
        editor.stops.size() > SIZE_MAX / sizeof(Range)) {
        return failure(error, Code::Limit, "Editor undo snapshot is too large");
    }
    EditorUndoState *snapshot = static_cast<EditorUndoState *>(malloc(sizeof(EditorUndoState)));
    if (!snapshot) return failure(error, Code::NoMemory, "Unable to allocate undo snapshot");
    snapshot->source = static_cast<char *>(malloc(editor.source.size() + 1));
    snapshot->stops = editor.stops.size()
        ? static_cast<Range *>(malloc(editor.stops.size() * sizeof(Range)))
        : 0;
    if (!snapshot->source || (editor.stops.size() && !snapshot->stops)) {
        free(snapshot->source);
        free(snapshot->stops);
        free(snapshot);
        return failure(error, Code::NoMemory, "Unable to copy editor undo snapshot");
    }
    snapshot->previous = editor.undo_top;
    snapshot->source_size = editor.source.size();
    snapshot->stop_count = editor.stops.size();
    snapshot->active_stop = editor.active_stop;
    if (snapshot->source_size) {
        memcpy(snapshot->source, editor.source.c_str(), snapshot->source_size);
    }
    snapshot->source[snapshot->source_size] = '\0';
    for (size_t index = 0; index < snapshot->stop_count; ++index) {
        snapshot->stops[index] = editor.stops[index];
    }
    editor.undo_top = snapshot;
    ++editor.undo_depth;
    return true;
}

void free_undo_state(EditorUndoState *state) {
    if (!state) return;
    free(state->source);
    free(state->stops);
    free(state);
}

void clear_undo(EditorCore &editor) {
    EditorUndoState *state = editor.undo_top;
    while (state) {
        EditorUndoState *previous = state->previous;
        free_undo_state(state);
        state = previous;
    }
    editor.undo_top = 0;
    editor.undo_depth = 0;
}

void trim_undo(EditorCore &editor) {
    if (editor.undo_depth <= kUndoLimit) return;
    EditorUndoState *state = editor.undo_top;
    while (state && state->previous && state->previous->previous) {
        state = state->previous;
    }
    if (state && state->previous) {
        free_undo_state(state->previous);
        state->previous = 0;
        editor.undo_depth = kUndoLimit;
    }
}

bool add_shifted_stop(Array<Range> &stops, Range stop, size_t shift,
                      Error &error) {
    if (stop.begin > SIZE_MAX - shift || stop.end > SIZE_MAX - shift) {
        return failure(error, Code::Limit, "Placeholder byte range overflow");
    }
    stop.begin += shift;
    stop.end += shift;
    if (stop.begin >= stop.end || !stops.push(stop)) {
        return failure(error, Code::NoMemory, "Unable to allocate placeholder range");
    }
    return true;
}

bool add_delta_stop(Array<Range> &stops, Range stop, size_t inserted,
                    size_t removed, Error &error) {
    if (inserted >= removed) {
        const size_t delta = inserted - removed;
        if (stop.begin > SIZE_MAX - delta || stop.end > SIZE_MAX - delta) {
            return failure(error, Code::Limit, "Placeholder byte range overflow");
        }
        stop.begin += delta;
        stop.end += delta;
    } else {
        const size_t delta = removed - inserted;
        if (stop.begin < delta || stop.end < delta) {
            return failure(error, Code::Format, "Placeholder shift underflow");
        }
        stop.begin -= delta;
        stop.end -= delta;
    }
    if (!stops.push(stop)) {
        return failure(error, Code::NoMemory, "Unable to allocate placeholder range");
    }
    return true;
}

bool make_spliced_text(const Text &source, Range range, const Text &inserted,
                       Text &out_text, Error &error) {
    if (!valid_range(source.c_str(), source.size(), range, error)) return false;
    const size_t kept_size = source.size() - (range.end - range.begin);
    if (kept_size > kMaxSourceBytes || inserted.size() > kMaxSourceBytes - kept_size) {
        return failure(error, Code::Limit, "Inserted source is too large");
    }
    const size_t result_size = kept_size + inserted.size();
    char *result = static_cast<char *>(malloc(result_size + 1));
    if (!result) return failure(error, Code::NoMemory, "Unable to allocate inserted source");
    size_t written = 0;
    const bool assembled = append_bytes(result, result_size, written,
                                        source.c_str(), range.begin) &&
                           append_bytes(result, result_size, written,
                                        inserted.c_str(), inserted.size()) &&
                           append_bytes(result, result_size, written,
                                        source.c_str() + range.end,
                                        source.size() - range.end);
    if (!assembled || written != result_size) {
        free(result);
        return failure(error, Code::Format, "Unable to assemble inserted source");
    }
    result[result_size] = '\0';
    const bool stored = out_text.set(result, result_size);
    free(result);
    if (!stored) {
        const Code code = result_size > kMaxTextBytes ? Code::Limit : Code::NoMemory;
        return failure(error, code, "Unable to store inserted source");
    }
    return true;
}

bool slice_equals(const char *source, size_t begin, size_t end,
                  const char *expected) {
    const size_t expected_size = strlen(expected);
    return end - begin == expected_size &&
           memcmp(source + begin, expected, expected_size) == 0;
}

bool is_alpha(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

size_t utf8_character_size(uint8_t first) {
    if (first <= 0x7f) return 1;
    if (first <= 0xdf) return 2;
    if (first <= 0xef) return 3;
    return 4;
}

bool supported_environment(const char *source, size_t begin, size_t end) {
    return slice_equals(source, begin, end, "matrix") ||
           slice_equals(source, begin, end, "pmatrix") ||
           slice_equals(source, begin, end, "bmatrix") ||
           slice_equals(source, begin, end, "cases") ||
           slice_equals(source, begin, end, "aligned");
}

bool supported_command(const char *source, size_t begin, size_t end) {
    static const char *const kExtra[] = {
        "\\begin", "\\end", "\\displaystyle", "\\left", "\\right",
        "\\vec", "\\hat", "\\overline", "\\sqrt", "\\frac",
        "\\sin", "\\cos", "\\tan", "\\log", "\\ln", "\\exp",
        "\\min", "\\max", "\\sum", "\\prod", "\\int", "\\iint",
        "\\iiint", "\\oint", "\\lim", "\\text", "\\mathrm",
        "\\mathbf", "\\mathit", "\\langle", "\\rangle", "\\lfloor",
        "\\rfloor", "\\lceil", "\\rceil", "\\{", "\\}", "\\|",
        "\\\\", "\\%", "\\#", "\\_"
    };
    for (size_t index = 0; index < sizeof(kExtra) / sizeof(kExtra[0]); ++index) {
        if (slice_equals(source, begin, end, kExtra[index])) return true;
    }
    size_t count = 0;
    const ToolEntry *entries = catalog_entries(&count);
    for (size_t index = 0; index < count; ++index) {
        if (slice_equals(source, begin, end, entries[index].command)) return true;
    }
    return false;
}

bool emit_token(LexToken *tokens, size_t capacity, size_t &count,
                TokenKind kind, size_t begin, size_t end, Error &error) {
    if (count >= capacity || !tokens) {
        return failure(error, Code::Limit, "Lexer token output capacity is too small",
                       begin, end);
    }
    tokens[count++] = LexToken{kind, Range{begin, end}};
    return true;
}

bool check_environment_after_command(const char *source, size_t length,
                                     size_t command_end, size_t &error_end) {
    size_t cursor = command_end;
    while (cursor < length && (source[cursor] == ' ' || source[cursor] == '\t' ||
                               source[cursor] == '\r' || source[cursor] == '\n')) {
        ++cursor;
    }
    if (cursor >= length || source[cursor] != '{') {
        error_end = command_end;
        return false;
    }
    const size_t env_begin = ++cursor;
    while (cursor < length && source[cursor] != '}') ++cursor;
    if (cursor >= length) {
        error_end = length;
        return false;
    }
    error_end = cursor + 1;
    return supported_environment(source, env_begin, cursor);
}

char expected_close(char open) {
    if (open == '(') return ')';
    if (open == '[') return ']';
    return '}';
}

bool opening_bracket(char value) {
    return value == '(' || value == '[' || value == '{';
}

bool closing_bracket(char value) {
    return value == ')' || value == ']' || value == '}';
}

} // namespace

EditorCore::EditorCore() noexcept
    : active_stop(kNoStop), undo_top(0), undo_depth(0) {}

EditorCore::~EditorCore() {
    clear_undo(*this);
}

bool lex_source(const char *source, size_t length, LexToken *out_tokens,
                size_t capacity, size_t &out_count, Error &error) {
    error.clear();
    out_count = 0;
    if (!source && length) return failure(error, Code::Format, "Lexer source is null");
    if (length > kMaxSourceBytes) {
        return failure(error, Code::Limit, "Lexer input is too large");
    }
    if (!valid_utf8(source, length, error)) return false;
    size_t *open_tokens = length
        ? static_cast<size_t *>(malloc(length * sizeof(size_t)))
        : 0;
    if (length && !open_tokens) {
        return failure(error, Code::NoMemory, "Unable to allocate lexer state");
    }
    size_t open_count = 0;
    size_t cursor = 0;
    while (cursor < length) {
        const size_t begin = cursor;
        const char value = source[cursor];
        if (value == '%') {
            while (cursor < length && source[cursor] != '\n') ++cursor;
            if (!emit_token(out_tokens, capacity, out_count, TokenKind::Comment,
                            begin, cursor, error)) {
                free(open_tokens);
                return false;
            }
            continue;
        }
        if (value == '{') {
            ++cursor;
            if (!emit_token(out_tokens, capacity, out_count, TokenKind::GroupOpen,
                            begin, cursor, error)) {
                free(open_tokens);
                return false;
            }
            open_tokens[open_count++] = out_count - 1;
            continue;
        }
        if (value == '}') {
            ++cursor;
            if (open_count) {
                --open_count;
                if (!emit_token(out_tokens, capacity, out_count, TokenKind::GroupClose,
                                begin, cursor, error)) {
                    free(open_tokens);
                    return false;
                }
            } else if (!emit_token(out_tokens, capacity, out_count, TokenKind::Error,
                                   begin, cursor, error)) {
                free(open_tokens);
                return false;
            }
            continue;
        }
        if (value == '\\') {
            ++cursor;
            if (cursor < length && is_alpha(source[cursor])) {
                while (cursor < length && is_alpha(source[cursor])) ++cursor;
            } else if (cursor < length) {
                cursor += utf8_character_size(static_cast<uint8_t>(source[cursor]));
            }
            bool supported = supported_command(source, begin, cursor);
            if (slice_equals(source, begin, cursor, "\\begin") ||
                slice_equals(source, begin, cursor, "\\end")) {
                size_t error_end = cursor;
                if (!check_environment_after_command(source, length, cursor, error_end)) {
                    cursor = error_end;
                    if (!emit_token(out_tokens, capacity, out_count, TokenKind::Error,
                                    begin, cursor, error)) {
                        free(open_tokens);
                        return false;
                    }
                    continue;
                }
            }
            if (!supported) {
                if (!emit_token(out_tokens, capacity, out_count, TokenKind::Error,
                                begin, cursor, error)) {
                    free(open_tokens);
                    return false;
                }
            } else if (!emit_token(out_tokens, capacity, out_count, TokenKind::Command,
                                   begin, cursor, error)) {
                free(open_tokens);
                return false;
            }
            continue;
        }

        while (cursor < length && source[cursor] != '%' && source[cursor] != '\\' &&
               source[cursor] != '{' && source[cursor] != '}') {
            ++cursor;
        }
        if (!emit_token(out_tokens, capacity, out_count, TokenKind::Text,
                        begin, cursor, error)) {
            free(open_tokens);
            return false;
        }
    }
    for (size_t index = 0; index < open_count; ++index) {
        out_tokens[open_tokens[index]].kind = TokenKind::Error;
    }
    free(open_tokens);
    return true;
}

bool find_text(const char *source, const char *needle, size_t start,
               Range &out_range, Error &error) {
    error.clear();
    if (!source || !needle) return failure(error, Code::Format, "Search text is null");
    size_t source_size = 0;
    size_t needle_size = 0;
    if (!source_length(source, source_size, error) ||
        !source_length(needle, needle_size, error)) return false;
    if (!needle_size) return failure(error, Code::Format, "Search text is empty");
    if (!valid_utf8(source, source_size, error) ||
        !valid_utf8(needle, needle_size, error)) return false;
    if (start > source_size || !is_boundary(source, source_size, start)) {
        return failure(error, Code::Format, "Search start is not a UTF-8 boundary",
                       start, start);
    }
    if (needle_size > source_size - start) {
        return failure(error, Code::NotFound, "Search text was not found");
    }
    for (size_t offset = start; offset <= source_size - needle_size; ++offset) {
        if (!is_boundary(source, source_size, offset) ||
            !is_boundary(source, source_size, offset + needle_size)) continue;
        if (memcmp(source + offset, needle, needle_size) == 0) {
            out_range = Range{offset, offset + needle_size};
            return true;
        }
    }
    return failure(error, Code::NotFound, "Search text was not found");
}

bool replace_text(Text &source, Range range, const char *replacement,
                  Error &error) {
    error.clear();
    size_t replacement_size = 0;
    if (!source_length(replacement, replacement_size, error)) return false;
    Text result;
    if (!make_replaced_text(source.c_str(), source.size(), range,
                            replacement, replacement_size, result, error)) return false;
    source.swap(result);
    return true;
}

bool matching_bracket(const char *source, size_t offset, Range &out_range,
                      Error &error) {
    error.clear();
    if (!source) return failure(error, Code::Format, "Bracket source is null");
    size_t length = 0;
    if (!source_length(source, length, error)) return false;
    if (!valid_utf8(source, length, error)) return false;
    if (offset >= length || !is_boundary(source, length, offset) ||
        (!opening_bracket(source[offset]) && !closing_bracket(source[offset]))) {
        return failure(error, Code::Format, "Offset does not identify a bracket",
                       offset, offset + (offset < length ? 1 : 0));
    }
    size_t *stack = length ? static_cast<size_t *>(malloc(length * sizeof(size_t))) : 0;
    if (length && !stack) {
        return failure(error, Code::NoMemory, "Unable to allocate bracket matcher state");
    }
    size_t stack_size = 0;
    size_t cursor = 0;
    bool found_target = false;
    bool found_match = false;
    size_t match = 0;
    while (cursor < length) {
        const char value = source[cursor];
        if (value == '%') {
            while (cursor < length && source[cursor] != '\n') ++cursor;
            continue;
        }
        if (value == '\\') {
            ++cursor;
            if (cursor >= length) break;
            if (is_alpha(source[cursor])) {
                while (cursor < length && is_alpha(source[cursor])) ++cursor;
            } else {
                cursor += utf8_character_size(static_cast<uint8_t>(source[cursor]));
            }
            continue;
        }
        if (opening_bracket(value)) {
            if (cursor == offset) found_target = true;
            stack[stack_size++] = cursor;
        } else if (closing_bracket(value)) {
            if (cursor == offset) found_target = true;
            if (stack_size && expected_close(source[stack[stack_size - 1]]) == value) {
                const size_t open_at = stack[--stack_size];
                if (open_at == offset) {
                    match = cursor;
                    found_match = true;
                } else if (cursor == offset) {
                    match = open_at;
                    found_match = true;
                }
            }
        }
        ++cursor;
    }
    free(stack);
    if (!found_target || !found_match) {
        return failure(error, Code::NotFound, "Matching bracket was not found",
                       offset, offset + 1);
    }
    out_range = Range{match, match + 1};
    return true;
}

bool editor_set_text(EditorCore &editor, const char *source, Error &error) {
    error.clear();
    if (!source) return failure(error, Code::Format, "Editor source is null");
    size_t source_size = 0;
    if (!source_length(source, source_size, error)) return false;
    if (!valid_utf8(source, source_size, error)) return false;
    Text replacement;
    if (!replacement.set(source, source_size)) {
        return failure(error, Code::NoMemory, "Unable to store editor source");
    }
    editor.source.swap(replacement);
    editor.stops.clear();
    editor.active_stop = kNoStop;
    clear_undo(editor);
    return true;
}

bool editor_insert_snippet(EditorCore &editor, Range range,
                           const Snippet &snippet, Error &error) {
    error.clear();
    if (!valid_range(editor.source.c_str(), editor.source.size(), range, error)) return false;
    if (snippet.source.empty() ||
        !valid_utf8(snippet.source.c_str(), snippet.source.size(), error)) {
        if (error.code == Code::None) {
            return failure(error, Code::Format, "Snippet source is empty");
        }
        return false;
    }
    size_t previous_end = 0;
    for (size_t index = 0; index < snippet.stops.size(); ++index) {
        const Range stop = snippet.stops[index];
        if (stop.begin >= stop.end || stop.end > snippet.source.size() ||
            !is_boundary(snippet.source.c_str(), snippet.source.size(), stop.begin) ||
            !is_boundary(snippet.source.c_str(), snippet.source.size(), stop.end) ||
            (index && stop.begin < previous_end)) {
            return failure(error, Code::Format, "Snippet placeholders are invalid",
                           stop.begin, stop.end);
        }
        previous_end = stop.end;
    }

    Text next_source;
    if (!make_spliced_text(editor.source, range, snippet.source, next_source, error)) return false;
    Array<Range> next_stops;
    for (size_t index = 0; index < editor.stops.size(); ++index) {
        const Range stop = editor.stops[index];
        if (stop.end <= range.begin) {
            if (!next_stops.push(stop)) {
                return failure(error, Code::NoMemory, "Unable to shift existing placeholders");
            }
        }
    }
    for (size_t index = 0; index < snippet.stops.size(); ++index) {
        if (!add_shifted_stop(next_stops, snippet.stops[index], range.begin, error)) return false;
    }
    const size_t removed = range.end - range.begin;
    for (size_t index = 0; index < editor.stops.size(); ++index) {
        const Range stop = editor.stops[index];
        if (stop.begin >= range.end && stop.end > range.end) {
            if (!add_delta_stop(next_stops, stop, snippet.source.size(), removed, error)) return false;
        }
    }
    if (!save_undo(editor, error)) return false;
    editor.source.swap(next_source);
    editor.stops.swap(next_stops);
    editor.active_stop = kNoStop;
    trim_undo(editor);
    return true;
}

bool editor_replace_text(EditorCore &editor, Range range,
                         const char *replacement, Error &error) {
    error.clear();
    if (!replacement) return failure(error, Code::Format, "Replacement text is null");
    size_t replacement_size = 0;
    if (!source_length(replacement, replacement_size, error)) return false;
    Text next_source;
    if (!make_replaced_text(editor.source.c_str(), editor.source.size(), range,
                            replacement, replacement_size, next_source, error)) return false;
    const size_t removed = range.end - range.begin;

    size_t preserved = kNoStop;
    if (editor.active_stop < editor.stops.size()) {
        const Range active = editor.stops[editor.active_stop];
        if (range.begin >= active.begin && range.end <= active.end) {
            preserved = editor.active_stop;
        }
    }
    if (preserved == kNoStop) {
        for (size_t index = 0; index < editor.stops.size(); ++index) {
            const Range stop = editor.stops[index];
            if (range.begin >= stop.begin && range.end <= stop.end) {
                preserved = index;
                break;
            }
        }
    }

    Array<Range> next_stops;
    size_t next_active = kNoStop;
    for (size_t index = 0; index < editor.stops.size(); ++index) {
        Range stop = editor.stops[index];
        if (index == preserved) {
            const size_t adjusted_end_base = stop.end - removed;
            if (replacement_size > SIZE_MAX - adjusted_end_base) {
                return failure(error, Code::Limit, "Placeholder byte range overflow");
            }
            stop.end = adjusted_end_base + replacement_size;
            if (stop.begin < stop.end) {
                if (!next_stops.push(stop)) {
                    return failure(error, Code::NoMemory, "Unable to update active placeholder");
                }
                next_active = next_stops.size() - 1;
            }
            continue;
        }
        const bool overlaps = range.begin < stop.end && range.end > stop.begin;
        if (overlaps) continue;
        if (stop.begin >= range.end && (removed || range.begin != range.end)) {
            if (!add_delta_stop(next_stops, stop, replacement_size, removed, error)) return false;
        } else if (range.begin == range.end && stop.begin >= range.begin) {
            if (!add_delta_stop(next_stops, stop, replacement_size, 0, error)) return false;
        } else if (!next_stops.push(stop)) {
            return failure(error, Code::NoMemory, "Unable to preserve placeholder range");
        }
        if (index == editor.active_stop) next_active = next_stops.size() - 1;
    }
    if (!save_undo(editor, error)) return false;
    editor.source.swap(next_source);
    editor.stops.swap(next_stops);
    editor.active_stop = next_active;
    trim_undo(editor);
    return true;
}

bool editor_tab(EditorCore &editor, bool backwards, bool &out_handled,
                Range &out_selection, Error &error) {
    error.clear();
    out_handled = false;
    out_selection = Range{0, 0};
    const size_t count = editor.stops.size();
    if (!count) {
        editor.active_stop = kNoStop;
        return true;
    }
    size_t next = kNoStop;
    if (editor.active_stop >= count) {
        next = backwards ? count - 1 : 0;
    } else if (backwards) {
        if (editor.active_stop > 0) next = editor.active_stop - 1;
    } else if (editor.active_stop + 1 < count) {
        next = editor.active_stop + 1;
    }
    if (next == kNoStop) {
        editor.active_stop = kNoStop;
        return true;
    }
    editor.active_stop = next;
    out_selection = editor.stops[next];
    out_handled = true;
    return true;
}

bool editor_undo(EditorCore &editor, Error &error) {
    error.clear();
    EditorUndoState *snapshot = editor.undo_top;
    if (!snapshot) return failure(error, Code::NotFound, "No editor change to undo");

    Text restored_source;
    if (!restored_source.set(snapshot->source, snapshot->source_size)) {
        const Code code = snapshot->source_size > kMaxTextBytes ? Code::Limit : Code::NoMemory;
        return failure(error, code, "Unable to restore editor source");
    }
    Array<Range> restored_stops;
    if (!restored_stops.resize(snapshot->stop_count)) {
        return failure(error, Code::NoMemory, "Unable to restore editor placeholders");
    }
    for (size_t index = 0; index < snapshot->stop_count; ++index) {
        restored_stops[index] = snapshot->stops[index];
    }

    editor.source.swap(restored_source);
    editor.stops.swap(restored_stops);
    editor.active_stop = snapshot->active_stop;
    editor.undo_top = snapshot->previous;
    --editor.undo_depth;
    free_undo_state(snapshot);
    return true;
}

} // namespace rm
