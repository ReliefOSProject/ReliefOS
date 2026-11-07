#ifndef RELIEFOS_FS_H
#define RELIEFOS_FS_H

#include <reliefos/fs_abi.h>

/**
 * @brief List at most the legacy API limit of directory entries via Linux getdents64.
 * @param path Required directory pathname.
 * @param entries Writable capacity-element array, nullable at zero capacity.
 * @param capacity Requested entries, capped at RELIEFOS_FS_MAX_ENTRIES.
 * @param out_count Optional completed count, zeroed on entry and assigned on success.
 * @return Zero or negative errno with errno set; names too large for the legacy
 * record fail with ENAMETOOLONG rather than returning an ambiguous truncated name.
 */
int reliefos_list_dir(const char *path, struct reliefos_dir_entry *entries, uint32_t capacity, uint32_t *out_count);
int reliefos_stat_legacy(const char *path, struct reliefos_stat *st);
int reliefos_fstat_legacy(int fd, struct reliefos_stat *st);
/**
 * @brief Read one directory entry while retaining the descriptor's opaque cookie.
 * @param fd Borrowed readable directory; callers serialize position changes on
 * the same open-file description, as for a shared POSIX directory stream.
 * @param entry Required writable output, unchanged on EOF or failure.
 * @return One entry, zero EOF, or negative errno with errno set. No global fd
 * cache is retained. Use POSIX readdir for names exceeding this legacy record.
 */
int reliefos_readdir(int fd, struct reliefos_dir_entry *entry);
int reliefos_fs_acl_get(const char *path, struct reliefos_fs_acl *acl);
int reliefos_fs_acl_set(const char *path, const struct reliefos_fs_acl *acl);
int reliefos_fs_acl_take_ownership(const char *path, struct reliefos_fs_acl *acl);
int reliefos_fs_acl_repair(const char *path, struct reliefos_fs_acl *acl);

#endif
