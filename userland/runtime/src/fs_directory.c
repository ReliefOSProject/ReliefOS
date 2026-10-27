/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefos/fs.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct directory_record {
    uint64_t inode;
    int64_t next;
    uint16_t length;
    uint8_t type;
    char name[256];
};
_Static_assert(offsetof(struct directory_record, name) == 19,
               "Linux x86_64 getdents64 wire layout");

/**
 * @brief Translate Linux directory metadata to the published application type.
 * @param fd Borrowed parent directory, stable during an unknown-type lookup.
 * @param record Validated terminated record from getdents64.
 * @param out Required writable application type.
 * @return Zero or negative errno; unknown types use fstatat without following links.
 */
static int directory_type(int fd, const struct directory_record *record, uint32_t *out)
{
    unsigned type = record->type;
    if (type == DT_UNKNOWN) {
        struct stat status;
        if (fstatat(fd, record->name, &status, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
        type = (status.st_mode & S_IFMT) >> 12;
    }
    switch (type) {
    case DT_REG: *out = RELIEFOS_FS_TYPE_FILE; break;
    case DT_DIR: *out = RELIEFOS_FS_TYPE_DIR; break;
    case DT_LNK: *out = RELIEFOS_FS_TYPE_SYMLINK; break;
    case DT_SOCK: *out = RELIEFOS_FS_TYPE_SOCKET; break;
    case DT_FIFO: *out = RELIEFOS_FS_TYPE_FIFO; break;
    default: *out = RELIEFOS_FS_TYPE_DEVICE; break;
    }
    return 0;
}

/**
 * @brief Read one application directory record using Linux getdents64 cookies.
 * @param fd Borrowed readable directory; caller serializes this shared OFD's position.
 * @param entry Required writable output; unchanged on EOF or error.
 * @return One record, zero EOF, or negative errno with errno set on failure.
 * No global descriptor cache is kept. A minimal buffer admits one record; if
 * a concurrent directory mutation makes it admit more, restore the first
 * record's opaque next cookie rather than losing the remaining entries.
 */
int reliefos_readdir(int fd, struct reliefos_dir_entry *entry)
{
    if (!entry) { errno = EFAULT; return -EFAULT; }
    struct directory_record record;
    const size_t header = offsetof(struct directory_record, name);
    size_t capacity = (header + 1u + 7u) & ~(size_t)7u;
    long got;
    for (;;) {
        got = syscall(SYS_getdents64, fd, &record, capacity);
        if (got >= 0 || errno != EINVAL || capacity >= sizeof(record)) break;
        capacity += 8u;
        if (capacity > sizeof(record)) capacity = sizeof(record);
    }
    if (got < 0) return -errno;
    if (!got) return 0;
    if ((size_t)got < header + 1u || record.length < header + 1u ||
        record.length > (size_t)got || record.length > capacity) {
        errno = EIO; return -EIO;
    }
    const char *end = memchr(record.name, 0, record.length - header);
    if (!end) { errno = EIO; return -EIO; }
    if (got > record.length && lseek(fd, record.next, SEEK_SET) < 0) return -errno;
    size_t length = (size_t)(end - record.name);
    if (!length) { errno = EIO; return -EIO; }
    if (length >= sizeof(entry->name)) { errno = ENAMETOOLONG; return -ENAMETOOLONG; }
    struct reliefos_dir_entry result = {0};
    int status = directory_type(fd, &record, &result.type);
    if (status) return status;
    memcpy(result.name, record.name, length + 1u);
    *entry = result;
    return 1;
}

/**
 * @brief Copy a bounded directory listing through the common Linux adapter.
 * @param path Required terminated directory pathname.
 * @param entries Writable capacity-element array, nullable at zero capacity.
 * @param capacity Requested entries, capped by the published legacy API limit.
 * @param out_count Optional output, zeroed on entry and assigned on success.
 * @return Zero or negative errno; closes only the descriptor opened by this call.
 */
int reliefos_list_dir(const char *path, struct reliefos_dir_entry *entries,
                     uint32_t capacity, uint32_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!path || (capacity && !entries)) { errno = EFAULT; return -EFAULT; }
    if (capacity > RELIEFOS_FS_MAX_ENTRIES) capacity = RELIEFOS_FS_MAX_ENTRIES;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    uint32_t count = 0;
    int result = 0;
    while (count < capacity) {
        result = reliefos_readdir(fd, &entries[count]);
        if (result <= 0) break;
        ++count;
    }
    int saved = errno;
    if (close(fd) < 0 && result >= 0) return -errno;
    if (result < 0) { errno = saved; return result; }
    if (out_count) *out_count = count;
    return 0;
}

/* Keep the published libleonos.so.2 aliases in the defining translation unit. */
extern __typeof__(reliefos_readdir) leonos_readdir __attribute__((alias("reliefos_readdir")));
extern __typeof__(reliefos_list_dir) leonos_list_dir __attribute__((alias("reliefos_list_dir")));
