#ifndef RELIEFMATH_CATALOG_H
#define RELIEFMATH_CATALOG_H

#include <stddef.h>

#include "snippet.h"

namespace rm {

struct ToolEntry {
    Tool tool;
    const char *key;
    const char *msgid;
    const char *category;
    const char *command;
    const char *insert;
    const char *parameters;
    const char *help_example;
};

/**
 * @brief Returns the complete immutable built-in mathematical tool catalog.
 * @param out_count Optional destination for the number of entries.
 * @return Stable pointer to the catalog table.
 */
const ToolEntry *catalog_entries(size_t *out_count);

/**
 * @brief Finds one catalog entry by its stable machine-readable key.
 * @param key Null-terminated stable key to match exactly.
 * @return Matching immutable entry, or null when the key is unknown.
 */
const ToolEntry *catalog_find(const char *key);

/**
 * @brief Searches English catalog metadata and commands without allocation.
 * @param query Null-terminated case-insensitive substring; empty matches all.
 * @param out_matches Optional caller array receiving up to capacity entries.
 * @param capacity Number of entry pointers writable in out_matches.
 * @return Number of matching entries copied, limited to capacity.
 */
size_t catalog_search(const char *query, const ToolEntry **out_matches,
                      size_t capacity);

/**
 * @brief Completes a backslash command prefix against the built-in catalog.
 * @param prefix Null-terminated command prefix beginning with a backslash.
 * @param out_matches Optional caller array receiving up to capacity entries.
 * @param capacity Number of entry pointers writable in out_matches.
 * @return Number of matching entries copied, limited to capacity.
 */
size_t complete_command(const char *prefix, const ToolEntry **out_matches,
                        size_t capacity);

} // namespace rm

#endif
