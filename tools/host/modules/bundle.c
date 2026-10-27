/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/bundle.h"
#include "tools/host/common/buffer.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(struct kernel_bundle *bundle, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(bundle->error, sizeof(bundle->error), format, arguments);
    va_end(arguments);
    return -1;
}

static bool ascii_alnum(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9');
}

bool kernel_tools_canonical_member(const char *name)
{
    if (!name || !*name || *name == '/') return false;
    const char *start = name;
    for (const char *at = name;; ++at) {
        unsigned char value = (unsigned char)*at;
        if (!value || value == '/') {
            size_t size = (size_t)(at - start);
            if (!size || (size == 1 && *start == '.') ||
                (size == 2 && start[0] == '.' && start[1] == '.')) return false;
            if (!value) return true;
            start = at + 1;
        } else if (!ascii_alnum(value) && value != '_' && value != '.' &&
                   value != '+' && value != '-') return false;
    }
}

/* Keep child I/O descriptors distinct from the standard streams even when
 * the tool was launched with one or more standard descriptors closed. */
static int safe_fd(int fd)
{
    if (fd < 0 || fd >= 3) return fd;
    int result = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

int kernel_bundle_open_member(int root_fd, const char *relative)
{
    if (!kernel_tools_canonical_member(relative)) { errno = EINVAL; return -1; }
    char *path = strdup(relative);
    if (!path) return -1;
    int current = fcntl(root_fd, F_DUPFD_CLOEXEC, 3);
    if (current < 0) { free(path); return -1; }
    char *component = path;
    for (;;) {
        char *slash = strchr(component, '/');
        if (slash) *slash = 0;
        if (!slash) {
            struct stat status;
            int result = fstatat(current, component, &status, AT_SYMLINK_NOFOLLOW);
            if (result || !S_ISREG(status.st_mode)) {
                int saved = result ? errno : EINVAL;
                close(current); free(path); errno = saved;
                return -1;
            }
        }
        int next = safe_fd(openat(current, component, O_RDONLY | O_CLOEXEC |
                                  O_NOFOLLOW | (slash ? O_DIRECTORY : O_NONBLOCK)));
        int saved = errno;
        close(current);
        if (next < 0) { free(path); errno = saved; return -1; }
        current = next;
        if (!slash) break;
        component = slash + 1;
    }
    free(path);
    struct stat status;
    int result = fstat(current, &status);
    if (result || !S_ISREG(status.st_mode)) {
        int saved = result ? errno : EINVAL;
        close(current);
        errno = saved ? saved : EINVAL;
        return -1;
    }
    return current;
}

int kernel_tools_read_all(int fd, struct byte_buffer *out)
{
    for (;;) {
        if (out->len > SIZE_MAX - 4097 || buffer_reserve(out, out->len + 4097)) return -1;
        ssize_t count = read(fd, out->data + out->len, 4096);
        if (count < 0) { if (errno == EINTR) continue; return -1; }
        if (!count) { out->data[out->len] = 0; return 0; }
        out->len += (size_t)count;
    }
}

static int read_member(struct kernel_bundle *bundle, const char *name, struct byte_buffer *out)
{
    int fd = kernel_bundle_open_member(bundle->root_fd, name);
    if (fd < 0) return fail(bundle, "missing or unsafe product: %s", name);
    int result = kernel_tools_read_all(fd, out);
    int saved = errno;
    close(fd);
    if (result) { errno = saved; return fail(bundle, "cannot read product: %s", name); }
    return 0;
}

static int read_text_member(struct kernel_bundle *bundle, const char *name, struct byte_buffer *out)
{
    if (read_member(bundle, name, out)) return -1;
    if (memchr(out->data, 0, out->len)) return fail(bundle, "embedded NUL in text product: %s", name);
    return 0;
}

/* The generic JSON parser accepts valid escaped U+0000. Install-manifest
 * strings and keys are C-string identifiers, so reject that value before any
 * identifier can be silently truncated. Escaped backslashes remain literal. */
bool kernel_tools_c_string_document(const struct byte_buffer *text)
{
    for (size_t i = 0; i < text->len; ++i) {
        if (!text->data[i]) return false;
        if (text->data[i] == '\\' && i + 1 < text->len) {
            if (text->data[i + 1] == 'u' && text->len - i >= 6 &&
                !memcmp(text->data + i + 2, "0000", 4)) return false;
            ++i;
        }
    }
    return true;
}

bool kernel_tools_digest_text(const char *text)
{
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

/* Hash the already-open regular inode through standard sha256sum stdin.
 * There is no shell, filename interpolation, second pathname lookup or host
 * module operation. The same checked inode remains held until child completion. */
int kernel_tools_sha256_fd(int fd, char digest[65])
{
    int pipe_fds[2];
    if (pipe(pipe_fds)) return -1;
    pipe_fds[0] = safe_fd(pipe_fds[0]);
    pipe_fds[1] = safe_fd(pipe_fds[1]);
    if (pipe_fds[0] < 0 || pipe_fds[1] < 0) {
        if (pipe_fds[0] >= 0) close(pipe_fds[0]);
        if (pipe_fds[1] >= 0) close(pipe_fds[1]);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) { close(pipe_fds[0]); close(pipe_fds[1]); return -1; }
    if (!child) {
        if (dup2(fd, STDIN_FILENO) < 0 || dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipe_fds[0]); close(pipe_fds[1]);
        execlp("sha256sum", "sha256sum", (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    char output[128]; size_t used = 0; int result = 0;
    for (;;) {
        ssize_t count = read(pipe_fds[0], output + used, sizeof(output) - used);
        if (count < 0) { if (errno == EINTR) continue; result = -1; break; }
        if (!count) break;
        used += (size_t)count;
        if (used == sizeof(output)) { result = -1; break; }
    }
    close(pipe_fds[0]);
    int status; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) || used < 66 || result) return -1;
    memcpy(digest, output, 64); digest[64] = 0;
    return kernel_tools_digest_text(digest) ? 0 : -1;
}

const struct kernel_bundle_file *kernel_bundle_find(const struct kernel_bundle *bundle,
                                                   const char *relative)
{
    for (size_t i = 0; i < bundle->file_count; ++i)
        if (!strcmp(bundle->files[i].name, relative)) return &bundle->files[i];
    return NULL;
}

static bool module_file(const char *name)
{
    size_t length = strlen(name);
    return length >= 3 && !strcmp(name + length - 3, ".ko");
}

static int configuration(struct kernel_bundle *bundle)
{
    const json_value *expected = json_member(&bundle->document, "configuration");
    if (!expected || expected->type != JSON_OBJECT) return fail(bundle, "missing kernel configuration");
    bool *seen = calloc(expected->child_count ? expected->child_count : 1, sizeof(*seen));
    if (!seen) return fail(bundle, "configuration allocation failed");
    char name[sizeof(bundle->prefix) + 16];
    snprintf(name, sizeof(name), "%sconfig", bundle->prefix);
    struct byte_buffer text = {0};
    int result = read_text_member(bundle, name, &text);
    if (result) { free(seen); buffer_destroy(&text); return -1; }
    char *line = (char *)text.data;
    for (; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "CONFIG_", 7)) {
            char *equal = strchr(line + 7, '=');
            if (equal) {
                *equal++ = 0;
                size_t i;
                for (i = 0; i < expected->child_count; ++i)
                    if (!strcmp(expected->keys[i], line + 7)) break;
                if (i == expected->child_count || seen[i] ||
                    !json_text(&expected->children[i]) || strcmp(json_text(&expected->children[i]), equal)) {
                    result = fail(bundle, "configuration disagrees with installed metadata"); break;
                }
                seen[i] = true;
            }
        }
        line = next;
    }
    for (size_t i = 0; !result && i < expected->child_count; ++i)
        if (!seen[i]) result = fail(bundle, "configuration member is absent: %s", expected->keys[i]);
    free(seen); buffer_destroy(&text);
    return result;
}

static int modules_order(struct kernel_bundle *bundle)
{
    const json_value *modules = json_member(&bundle->document, "modules");
    if (!modules || modules->type != JSON_ARRAY) return fail(bundle, "invalid module inventory");
    size_t actual = 0;
    for (size_t i = 0; i < bundle->file_count; ++i) if (module_file(bundle->files[i].name)) ++actual;
    if (actual != modules->child_count) return fail(bundle, "module set disagrees with installed inventory");
    for (size_t i = 0; i < modules->child_count; ++i) {
        const char *member = json_text(&modules->children[i]);
        if (!member || !module_file(member) || !kernel_bundle_find(bundle, member))
            return fail(bundle, "module inventory contains an absent product");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(member, json_text(&modules->children[j]))) return fail(bundle, "duplicate module inventory member");
    }
    const json_value *config = json_member(&bundle->document, "configuration");
    const char *enabled = json_text(json_member(config, "MODULES"));
    if (actual && (!enabled || strcmp(enabled, "y"))) return fail(bundle, "external modules require CONFIG_MODULES=y");
    char name[sizeof(bundle->prefix) + 32];
    snprintf(name, sizeof(name), "%smodules.order", bundle->prefix);
    struct byte_buffer text = {0};
    int result = read_text_member(bundle, name, &text);
    bool *seen = calloc(actual ? actual : 1, sizeof(*seen));
    if (!seen) result = fail(bundle, "module order allocation failed");
    if (result) { free(seen); buffer_destroy(&text); return -1; }
    size_t count = 0; char *line = (char *)text.data;
    while (line && *line) {
        char *next = strchr(line, '\n'); if (next) *next++ = 0;
        if (!kernel_tools_canonical_member(line)) { result = fail(bundle, "noncanonical installed module order"); break; }
        size_t i;
        for (i = 0; i < actual; ++i) {
            const char *member = json_text(&modules->children[i]);
            if (!strncmp(member, bundle->prefix, strlen(bundle->prefix)) &&
                !strcmp(member + strlen(bundle->prefix), line)) break;
        }
        if (i == actual || seen[i]) { result = fail(bundle, "module order disagrees with installed inventory"); break; }
        seen[i] = true; ++count; line = next;
        if (line && !*line && next && (size_t)(line - (char *)text.data) < text.len)
            result = fail(bundle, "empty installed module order member");
        if (result) break;
    }
    if (!result && count != actual) result = fail(bundle, "incomplete installed module order");
    free(seen); buffer_destroy(&text); bundle->module_count = actual;
    return result;
}

static int identity(struct kernel_bundle *bundle)
{
    long schema;
    const json_value *root = &bundle->document;
    const char *arch = json_text(json_member(root, "arch"));
    bundle->release = json_text(json_member(root, "release"));
    bundle->abi = json_text(json_member(root, "module_abi"));
    if (json_int(json_member(root, "schema"), &schema) || schema != 1 || !arch || strcmp(arch, "x86_64"))
        return fail(bundle, "unsupported kernel install bundle");
    const char *release = bundle->release;
    size_t length = release ? strlen(release) : 0;
    if (!length || length > 31 || !ascii_alnum((unsigned char)release[0]) || !kernel_tools_digest_text(bundle->abi))
        return fail(bundle, "invalid kernel release or module ABI");
    for (size_t i = 1; i < length; ++i)
        if (!ascii_alnum((unsigned char)release[i]) && !strchr("._+-", release[i]))
            return fail(bundle, "unsafe kernel release");
    if (length < 12 || strncmp(release + length - 12, bundle->abi, 12))
        return fail(bundle, "kernel release does not bind its module ABI");
    snprintf(bundle->prefix, sizeof(bundle->prefix), "lib/modules/%s/", release);
    const char *directory = json_text(json_member(root, "module_directory"));
    if (!directory || strlen(directory) + 1 != strlen(bundle->prefix) ||
        strncmp(directory, bundle->prefix, strlen(directory)))
        return fail(bundle, "inconsistent installed module directory");
    return 0;
}

int kernel_bundle_open(struct kernel_bundle *bundle, const char *directory,
                       size_t requirement_count, const char *const requirements[])
{
    bundle->root_fd = safe_fd(open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (bundle->root_fd < 0) return fail(bundle, "explicit regular installation directory is required");
    struct stat root, filesystem_root;
    if (fstat(bundle->root_fd, &root) || stat("/", &filesystem_root) ||
        (root.st_dev == filesystem_root.st_dev && root.st_ino == filesystem_root.st_ino))
        return fail(bundle, "explicit installation prefix is required");
    struct byte_buffer text = {0};
    int result = read_member(bundle, "install.bundle.json", &text);
    if (!result && !kernel_tools_c_string_document(&text)) result = fail(bundle, "embedded NUL in manifest identifiers");
    if (!result) result = json_parse((const char *)text.data, text.len, &bundle->document,
                                     bundle->error, sizeof(bundle->error));
    buffer_destroy(&text);
    if (result || identity(bundle)) return -1;
    const json_value *files = json_member(&bundle->document, "files");
    if (!files || files->type != JSON_OBJECT) return fail(bundle, "missing installed product map");
    if (files->child_count > SIZE_MAX / sizeof(*bundle->files)) return fail(bundle, "product map size overflow");
    bundle->files = calloc(files->child_count ? files->child_count : 1, sizeof(*bundle->files));
    if (!bundle->files) return fail(bundle, "product map allocation failed");
    bundle->file_count = files->child_count;
    for (size_t i = 0; i < bundle->file_count; ++i) {
        struct kernel_bundle_file *file = &bundle->files[i];
        file->name = files->keys[i];
        const json_value *properties = &files->children[i];
        file->sha256 = json_text(json_member(properties, "sha256"));
        long size;
        if (!kernel_tools_canonical_member(file->name) || !kernel_tools_digest_text(file->sha256) ||
            json_int(json_member(properties, "size"), &size) || size < 0)
            return fail(bundle, "invalid installed product record: %s", file->name);
        if (strcmp(file->name, "kernel.sys") && strcmp(file->name, "kernel.debug") &&
            strcmp(file->name, "kerneldebug.sys") && strncmp(file->name, bundle->prefix, strlen(bundle->prefix)))
            return fail(bundle, "product outside installed release: %s", file->name);
        file->size = (off_t)size;
        if ((long)file->size != size) return fail(bundle, "product size overflow: %s", file->name);
        int fd = kernel_bundle_open_member(bundle->root_fd, file->name);
        if (fd < 0) return fail(bundle, "missing or unsafe installed product: %s", file->name);
        struct stat status; char digest[65];
        result = fstat(fd, &status) || status.st_size != file->size || kernel_tools_sha256_fd(fd, digest) || strcmp(digest, file->sha256);
        close(fd);
        if (result) return fail(bundle, "bundle checksum mismatch: %s", file->name);
    }
    static const char *const required[] = {"config", "module.abi", "Module.symvers", "System.map",
        "modules.order", "modules.builtin", "modules.builtin.modinfo"};
    if (!kernel_bundle_find(bundle, "kernel.sys") || !kernel_bundle_find(bundle, "kernel.debug") ||
        !kernel_bundle_find(bundle, "kerneldebug.sys")) return fail(bundle, "incomplete installed kernel products");
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char name[sizeof(bundle->prefix) + 32];
        snprintf(name, sizeof(name), "%s%s", bundle->prefix, required[i]);
        if (!kernel_bundle_find(bundle, name)) return fail(bundle, "incomplete installed kernel metadata: %s", name);
    }
    char name[sizeof(bundle->prefix) + 16];
    snprintf(name, sizeof(name), "%smodule.abi", bundle->prefix);
    result = read_text_member(bundle, name, &text);
    if (result) { buffer_destroy(&text); return -1; }
    char *abi = (char *)text.data;
    while (*abi && isspace((unsigned char)*abi)) ++abi;
    size_t length = strlen(abi);
    while (length && isspace((unsigned char)abi[length - 1])) abi[--length] = 0;
    result = strcmp(abi, bundle->abi);
    buffer_destroy(&text);
    if (result) return fail(bundle, "installed module ABI disagrees with metadata");
    if (configuration(bundle) || modules_order(bundle)) return -1;
    const json_value *config = json_member(&bundle->document, "configuration");
    for (size_t i = 0; i < requirement_count; ++i) {
        const char *name_required = requirements[i];
        if (!*name_required) return fail(bundle, "empty required boot feature");
        for (const char *at = name_required; *at; ++at)
            if (!((*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9') || *at == '_'))
                return fail(bundle, "invalid required boot feature: %s", name_required);
        const char *value = json_text(json_member(config, name_required));
        if (!value || (strcmp(value, "y") && strcmp(value, "m")))
            return fail(bundle, "required boot feature is disabled: CONFIG_%s", name_required);
    }
    return 0;
}

void kernel_bundle_close(struct kernel_bundle *bundle)
{
    if (bundle->root_fd >= 0) close(bundle->root_fd);
    bundle->root_fd = -1;
    free(bundle->files); bundle->files = NULL; bundle->file_count = 0;
    json_free(&bundle->document);
    bundle->release = bundle->abi = NULL;
}
