#ifndef RELIEFMATH_LIBRARY_H
#define RELIEFMATH_LIBRARY_H

#include "catalog.h"

namespace rm {

enum class FavoriteCloseDecision {
    None,
    WaitForIo,
    Save,
    Close,
    ConfirmDiscard
};

/** Pure close/save decision for the asynchronous favorites dialog. */
FavoriteCloseDecision favorite_close_decision(bool close_requested,
                                               bool io_pending, bool dirty,
                                               bool save_failed) noexcept;

/** Maximum number of columns accepted by the structure wizard for a tool. */
unsigned structure_column_limit(Tool tool) noexcept;

/** Finds a command prefix at the caret and the complete command range to replace. */
bool command_completion_prefix(const char *source, size_t caret, Text &prefix,
                              Range &replace_range, Error &error) noexcept;

/** Replaces every non-overlapping UTF-8 match, committing output only on success. */
bool replace_all_utf8(const char *source, const char *needle,
                      const char *replacement, Text &output,
                      size_t &replace_count, Error &error) noexcept;

/** Returns true when a catalog entry matches its metadata or Chinese search aliases. */
bool catalog_entry_matches(const ToolEntry &entry, const char *localized_name,
                           const char *localized_category,
                           const char *query) noexcept;

/** Chinese category alias used as search data. */
const char *catalog_category_zh(const char *category) noexcept;

} // namespace rm

#endif
