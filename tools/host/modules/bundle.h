/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_HOST_MODULE_BUNDLE_H
#define RELIEFOS_HOST_MODULE_BUNDLE_H
#include "tools/host/manifest/json.h"
#include <stddef.h>
#include <stdbool.h>
#include "tools/host/common/buffer.h"
#include <sys/types.h>

struct kernel_bundle_file {
    const char *name, *sha256;
    off_t size;
};
/* Strings and file names borrow from document until kernel_bundle_close(). */
struct kernel_bundle {
    int root_fd;
    json_value document;
    const char *release, *abi;
    char prefix[64], error[256];
    struct kernel_bundle_file *files;
    size_t file_count, module_count;
};
/** Open and validate a kernel-owned install manifest and every listed product.
 * The caller initializes root_fd to -1 and the remaining fields to zero.
 * On failure, error retains a diagnostic; close is safe after either outcome.
 * This operation is read-only. Product opens refuse every symlink component.
 */
int kernel_bundle_open(struct kernel_bundle *bundle, const char *directory,
                       size_t requirement_count, const char *const requirements[]);
/** Release all owned descriptors, parsed JSON and product indexes. */
void kernel_bundle_close(struct kernel_bundle *bundle);
/** Return a borrowed manifest product or NULL, without opening any pathname. */
const struct kernel_bundle_file *kernel_bundle_find(const struct kernel_bundle *bundle,
                                                   const char *relative);
/** Open one canonical regular member below a held root, refusing all symlinks.
 * Returns an owned descriptor, or -1 with errno; no path escapes the held root.
 */
int kernel_bundle_open_member(int root_fd, const char *relative);
/** Validate canonical, relative product names, without filesystem access. */
bool kernel_tools_canonical_member(const char *name);
/** Read an open descriptor into an owned, NUL-terminated byte buffer. */
int kernel_tools_read_all(int fd, struct byte_buffer *out);
/** Hash the held regular descriptor through sha256sum stdin, without a shell. */
int kernel_tools_sha256_fd(int fd, char digest[65]);
/** Recognize exactly 64 lowercase hexadecimal SHA-256 digits. */
bool kernel_tools_digest_text(const char *text);
/** Reject logical embedded NUL identifiers before the C-string JSON reader. */
bool kernel_tools_c_string_document(const struct byte_buffer *text);
#endif
