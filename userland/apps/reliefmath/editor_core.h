#ifndef RELIEFMATH_EDITOR_CORE_H
#define RELIEFMATH_EDITOR_CORE_H

#include <stddef.h>

#include "catalog.h"
#include "snippet.h"

namespace rm {

enum class TokenKind : uint8_t {
    Text,
    Command,
    GroupOpen,
    GroupClose,
    Comment,
    Error
};

struct LexToken {
    TokenKind kind;
    Range range;
};

struct EditorUndoState;

struct EditorCore {
    Text source;
    Array<Range> stops;
    size_t active_stop;
    EditorUndoState *undo_top;
    size_t undo_depth;

    EditorCore() noexcept;
    ~EditorCore();
    EditorCore(const EditorCore &) = delete;
    EditorCore &operator=(const EditorCore &) = delete;
};

// Source, search text, replacement text, and edit results are limited to
// kMaxSourceBytes UTF-8 bytes. Limit checks precede UTF-8 validation and source
// buffer allocation. C-string length probing reads at most kMaxSourceBytes + 1
// bytes; failed edits keep state.

/**
 * @brief Lexes source into byte-ranged syntax styles and unsupported tokens.
 * @param source UTF-8 source bytes; null is accepted only for zero length.
 * @param length Number of source bytes.
 * @param out_tokens Caller array receiving tokens in source order.
 * @param capacity Number of writable tokens in out_tokens.
 * @param out_count Receives the number of tokens written.
 * @param error Caller-owned error state updated for invalid input or capacity.
 * @return True when scanning completed, including source containing error tokens.
 */
bool lex_source(const char *source, size_t length, LexToken *out_tokens,
                size_t capacity, size_t &out_count, Error &error);

/**
 * @brief Finds a UTF-8 substring at or after a codepoint boundary.
 * @param source Null-terminated source string to search.
 * @param needle Non-empty null-terminated UTF-8 text to locate.
 * @param start Starting byte offset, which must be a codepoint boundary.
 * @param out_range Receives the half-open byte range of the first match.
 * @param error Caller-owned error state updated on malformed or missing input.
 * @return True when a complete match is found.
 */
bool find_text(const char *source, const char *needle, size_t start,
               Range &out_range, Error &error);

/**
 * @brief Replaces a complete UTF-8 byte range without splitting codepoints.
 * @param source Mutable text replaced only after a complete result is built.
 * @param range Half-open byte range whose endpoints must be codepoint boundaries.
 * @param replacement Null-terminated UTF-8 replacement text.
 * @param error Caller-owned error state updated when validation or allocation fails.
 * @return True when replacement succeeds; false leaves source unchanged.
 */
bool replace_text(Text &source, Range range, const char *replacement,
                  Error &error);

/**
 * @brief Finds the matching nested bracket while skipping comments and escapes.
 * @param source Null-terminated UTF-8 source string.
 * @param offset Byte offset of an opening or closing bracket.
 * @param out_range Receives the other bracket's one-byte range.
 * @param error Caller-owned error state updated when no match exists.
 * @return True when a matching bracket is found.
 */
bool matching_bracket(const char *source, size_t offset, Range &out_range,
                      Error &error);

/**
 * @brief Initializes document text and clears placeholder and undo state.
 * @param editor Editor core to update.
 * @param source Null-terminated valid UTF-8 document source.
 * @param error Caller-owned error state updated when input cannot be stored.
 * @return True when the source is stored and transient editor state is reset.
 */
bool editor_set_text(EditorCore &editor, const char *source, Error &error);

/**
 * @brief Inserts a snippet, replaces intersecting stops, and records one undo unit.
 * @param editor Editor core receiving the source and shifted byte stops.
 * @param range Half-open source range replaced by the snippet.
 * @param snippet Validated snippet whose stops are relative to its own source.
 * @param error Caller-owned error state updated if validation or storage fails.
 * @return True when source and stops are updated together.
 */
bool editor_insert_snippet(EditorCore &editor, Range range,
                           const Snippet &snippet, Error &error);

/**
 * @brief Applies one ordinary-input or committed-IME edit as one undo unit.
 * @param editor Editor core whose text and placeholder ranges are transformed.
 * @param range Half-open source byte range being edited.
 * @param replacement Null-terminated valid UTF-8 replacement text.
 * @param error Caller-owned error state updated if validation or storage fails.
 * @return True when the edit is applied and affected stops are shifted or removed.
 */
bool editor_replace_text(EditorCore &editor, Range range,
                         const char *replacement, Error &error);

/**
 * @brief Selects the next or previous placeholder before yielding normal Tab focus.
 * @param editor Editor core whose active stop is changed.
 * @param backwards True for Shift-Tab; false for Tab.
 * @param out_handled Receives false when focus navigation should handle the key.
 * @param out_selection Receives the selected half-open placeholder range.
 * @param error Caller-owned error state updated only for an invalid editor state.
 * @return True when navigation was evaluated successfully.
 */
bool editor_tab(EditorCore &editor, bool backwards, bool &out_handled,
                Range &out_selection, Error &error);

/**
 * @brief Restores the most recent text and placeholder snapshot.
 * @param editor Editor core whose last edit is rolled back.
 * @param error Caller-owned error state updated when the history is empty.
 * @return True when a previous editor state is restored.
 */
bool editor_undo(EditorCore &editor, Error &error);

} // namespace rm

#endif
