/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned little16(const unsigned char *at) { return at[0] | (unsigned)at[1] << 8; }
static uint64_t little64(const unsigned char *at)
{
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= (uint64_t)at[i] << (i * 8);
    return result;
}

static int static_busybox(int fd)
{
    unsigned char header[64], program[56]; struct stat status;
    if (fstat(fd, &status) || pread(fd, header, sizeof(header), 0) != sizeof(header) ||
        memcmp(header, "\177ELF\002\001\001", 7) || little16(header + 18) != 62 || little16(header + 54) != 56) return -1;
    uint64_t offset = little64(header + 32); unsigned count = little16(header + 56);
    if (!count || offset > (uint64_t)status.st_size || (uint64_t)count * 56 > (uint64_t)status.st_size - offset) return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (pread(fd, program, sizeof(program), (off_t)(offset + (uint64_t)i * 56)) != sizeof(program) ||
            (program[0] == 3 && !program[1] && !program[2] && !program[3])) return -1;
    }
    return 0;
}

static int copy_input(const char *path, int tree, const char *target, mode_t mode, char digest[65])
{
    int fd = kernel_stage_input(path);
    if (fd < 0) return -1;
    int result = kernel_tools_sha256_fd(fd, digest) || lseek(fd, 0, SEEK_SET) < 0 ||
                 kernel_stage_copy_fd(fd, tree, target, digest, mode);
    close(fd);
    return result ? -1 : 0;
}

static bool applet_available(const struct byte_buffer *available, const char *name)
{
    size_t length = strlen(name);
    const char *at = (const char *)available->data, *end = at + available->len;
    while (at < end) {
        const char *newline = memchr(at, '\n', (size_t)(end - at));
        size_t size = newline ? (size_t)(newline - at) : (size_t)(end - at);
        if (size == length && !memcmp(at, name, size)) return true;
        at += size + (newline != NULL);
    }
    return false;
}

static int applets(int tree, const char *busybox, const char *links)
{
    struct byte_buffer available = {0}, paths = {0};
    char *arguments[] = {(char *)busybox, "--list", NULL};
    int result = kernel_stage_run(arguments, &available);
    int fd = result ? -1 : kernel_stage_input(links);
    if (fd < 0) result = -1;
    if (!result) result = kernel_tools_read_all(fd, &paths);
    if (fd >= 0) close(fd);
    if (!result && (memchr(paths.data, 0, paths.len) || memchr(available.data, 0, available.len))) result = -1;
    char *at = (char *)paths.data;
    while (!result && at && *at) {
        char *next = strchr(at, '\n'); if (next) *next++ = 0;
        if (*at == '/') ++at;
        if (strcmp(at, "bin/busybox") && strcmp(at, "init")) {
            const char *base = strrchr(at, '/'); base = base ? base + 1 : at;
            if (!kernel_stage_tree_member(at) || !applet_available(&available, base)) { result = -1; errno = EINVAL; break; }
            size_t depth = 0;
            for (const char *p = at; *p; ++p) if (*p == '/') ++depth;
            char *target = malloc(depth * 3 + sizeof("bin/busybox"));
            if (!target) { result = -1; break; }
            /* bin and sbin are siblings, while applets inside bin can use the
             * simple "busybox" target expected by portable BusyBox installs. */
            if (depth == 1 && !strncmp(at, "bin/", 4)) strcpy(target, "busybox");
            else {
                char *out = target;
                for (size_t i = 0; i < depth; ++i) { memcpy(out, "../", 3); out += 3; }
                strcpy(out, "bin/busybox");
            }
            result = kernel_stage_symlink(tree, at, target);
            free(target);
        }
        at = next;
    }
    buffer_destroy(&available); buffer_destroy(&paths);
    return result;
}

static int early_modules(int module_root, int tree, const json_value *metadata,
                          const char *prefix, struct byte_buffer *order)
{
    const json_value *names = json_member(metadata, "early_modules");
    const json_value *records = json_member(metadata, "metadata");
    const json_value *files = json_member(metadata, "files");
    if (!names || names->type != JSON_ARRAY || !records || records->type != JSON_OBJECT) return -1;
    for (size_t i = 0; i < names->child_count; ++i) {
        const char *name = json_text(&names->children[i]);
        const char *path = name ? json_text(json_member(json_member(records, name), "path")) : NULL;
        if (!path || strncmp(path, prefix, strlen(prefix)) || !kernel_tools_canonical_member(path)) return -1;
        const char *hash = json_text(json_member(json_member(files, path), "sha256"));
        if (kernel_stage_copy(module_root, path, tree, path, hash, 0644)) return -1;
        const char *relative = path + strlen(prefix); size_t length = strlen(relative);
        if (buffer_reserve(order, order->len + length + 1)) return -1;
        memcpy(order->data + order->len, relative, length); order->len += length; order->data[order->len++] = '\n';
    }
    static const char *const members[] = {"modules.builtin", "modules.builtin.modinfo", "module.abi"};
    for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); ++i) {
        char path[128]; snprintf(path, sizeof(path), "%s%s", prefix, members[i]);
        const char *hash = json_text(json_member(json_member(files, path), "sha256"));
        if (kernel_stage_copy(module_root, path, tree, path, hash, 0644)) return -1;
    }
    return 0;
}

static int boot_list(int tree, const json_value *metadata)
{
    const json_value *boot = json_member(metadata, "boot_modules");
    if (!boot || boot->type != JSON_ARRAY) return -1;
    struct byte_buffer text = {0}; int result = 0;
    for (size_t i = 0; i < boot->child_count; ++i) {
        const char *name = json_text(&boot->children[i]);
        if (!name || !kernel_tools_canonical_member(name) || strchr(name, '/') || buffer_reserve(&text, text.len + strlen(name) + 1)) {
            result = -1; break;
        }
        size_t size = strlen(name); memcpy(text.data + text.len, name, size); text.len += size; text.data[text.len++] = '\n';
    }
    if (!result) result = kernel_stage_write(tree, "etc/modules-load.d/reliefos-boot.conf", text.data, text.len, 0644);
    buffer_destroy(&text);
    return result;
}

static int output_metadata(struct kernel_stage *stage, const json_value *modules, const char *archive_hash,
                            const char *busybox_hash, const char *recovery_hash, unsigned epoch)
{
    char *text = NULL; size_t size = 0;
    FILE *stream = open_memstream(&text, &size);
    if (!stream) return -1;
    fprintf(stream, "{\"schema\":1,\"release\":\"%s\",\"module_abi\":\"%s\",\"kernel_sha256\":\"%s\","
                    "\"initramfs_sha256\":\"%s\",\"busybox_sha256\":\"%s\",\"recovery_sha256\":\"%s\",\"epoch\":%u,\"early_modules\":",
            json_text(json_member(modules, "release")), json_text(json_member(modules, "module_abi")),
            json_text(json_member(modules, "kernel_sha256")), archive_hash, busybox_hash, recovery_hash, epoch);
    int result = kernel_stage_json(stream, json_member(modules, "early_modules"));
    fputs("}\n", stream);
    if (ferror(stream)) result = -1;
    if (fclose(stream)) result = -1;
    if (!result) result = kernel_stage_write(stage->root_fd, "initramfs.json", text, size, 0644);
    free(text);
    return result;
}

static int publish_archive(struct kernel_stage *stage)
{
    char *metadata = malloc(strlen(stage->leaf) + sizeof(".json"));
    if (!metadata) return -1;
    sprintf(metadata, "%s.json", stage->leaf);
    const char *names[] = {stage->leaf, metadata}; int result = 0;
    for (size_t i = 0; i < 2; ++i) {
        struct stat status;
        int exists = fstatat(stage->parent_fd, names[i], &status, AT_SYMLINK_NOFOLLOW);
        if ((!exists && !S_ISREG(status.st_mode)) || (exists && errno != ENOENT)) { result = -1; errno = EINVAL; break; }
    }
    if (!result) result = renameat(stage->root_fd, "initramfs.cpio", stage->parent_fd, stage->leaf);
    if (!result) result = renameat(stage->root_fd, "initramfs.json", stage->parent_fd, metadata);
    if (!result) result = fsync(stage->parent_fd);
    free(metadata);
    return result;
}

int kernel_initramfs_stage(int argc, char **argv)
{
    if (argc != 7) return 2;
    int result = -1, module_root = -1, source = -1, tree = -1, box = -1, archive = -1;
    json_value modules = {0}; struct byte_buffer order = {0};
    struct kernel_stage stage = {.root_fd = -1, .parent_fd = -1};
    char *end; errno = 0; unsigned long long epoch = strtoull(argv[5], &end, 10);
    if (errno || !*argv[5] || *end || argv[5][0] == '-' || epoch > UINT32_MAX) goto out;
    box = kernel_stage_input(argv[2]);
    if (box < 0 || static_busybox(box)) { fputs("kernel-tools: early BusyBox must be a static x86_64 ELF\n", stderr); goto out; }
    close(box); box = -1;
    box = kernel_stage_input(argv[6]);
    if (box < 0 || static_busybox(box)) { fputs("kernel-tools: recovery tool must be a static x86_64 ELF\n", stderr); goto out; }
    close(box); box = -1;
    module_root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    source = open(argv[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (module_root < 0 || source < 0 || kernel_stage_document(module_root, "modules-stage.json", &modules)) goto out;
    const char *release, *abi, *kernel_hash;
    long schema;
    if (kernel_stage_identity(&modules, &release, &abi, &kernel_hash) ||
        json_int(json_member(&modules, "schema"), &schema) || schema != 1 ||
        kernel_stage_verify(module_root, json_member(&modules, "files")) || kernel_stage_begin(&stage, argv[4]) ||
        kernel_stage_mkdir(stage.root_fd, "root", 0755)) goto out;
    tree = openat(stage.root_fd, "root", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (tree < 0) goto out;
    static const char *const directories[] = {"bin", "sbin", "etc/modules-load.d", "dev", "proc", "sys", "run", "newroot", "tmp"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        if (kernel_stage_mkdir(tree, directories[i], i == 8 ? 01777 : 0755)) goto out;
    char busybox_hash[65], recovery_hash[65], digest[65];
    if (copy_input(argv[2], tree, "bin/busybox", 0755, busybox_hash)) goto out;
    if (copy_input(argv[6], tree, "sbin/kernel-bundle", 0755, recovery_hash)) goto out;
    const char *source_names[] = {"system/initramfs/init", "system/initramfs/functions"};
    const char *target_names[] = {"init", "etc/initramfs-functions"};
    for (size_t i = 0; i < 2; ++i) {
        int fd = kernel_bundle_open_member(source, source_names[i]);
        if (fd < 0) goto out;
        int failed = kernel_tools_sha256_fd(fd, digest) || lseek(fd, 0, SEEK_SET) < 0 ||
                     kernel_stage_copy_fd(fd, tree, target_names[i], digest, i ? 0644 : 0755);
        close(fd);
        if (failed) goto out;
    }
    if (applets(tree, argv[2], argv[3])) goto out;
    char prefix[64], path[100]; snprintf(prefix, sizeof(prefix), "lib/modules/%s/", release);
    if (early_modules(module_root, tree, &modules, prefix, &order)) goto out;
    snprintf(path, sizeof(path), "%smodules.order", prefix);
    if (kernel_stage_write(tree, path, order.data, order.len, 0644)) goto out;
    char *tree_path = malloc(strlen(stage.path) + sizeof("/root"));
    if (!tree_path) goto out;
    sprintf(tree_path, "%s/root", stage.path);
    char *arguments[] = {argv[2], "depmod", "-b", tree_path, (char *)release, NULL};
    int failed = kernel_stage_run(arguments, NULL); free(tree_path);
    if (failed) goto out;
    char release_line[34]; int length = snprintf(release_line, sizeof(release_line), "%s\n", release);
    if (kernel_stage_write(tree, "etc/kernel-release", release_line, (size_t)length, 0644) ||
        boot_list(tree, &modules) || kernel_stage_symlink(tree, "etc/mtab", "/proc/mounts")) goto out;
    archive = openat(stage.root_fd, "initramfs.cpio", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (archive < 0 || kernel_stage_newc(tree, archive, (unsigned)epoch) || fsync(archive)) goto out;
    close(archive); archive = kernel_bundle_open_member(stage.root_fd, "initramfs.cpio");
    if (archive < 0 || kernel_tools_sha256_fd(archive, digest) ||
        output_metadata(&stage, &modules, digest, busybox_hash, recovery_hash, (unsigned)epoch) || publish_archive(&stage)) goto out;
    result = 0;
out:
    if (result) fprintf(stderr, "kernel-tools: initramfs stage failed: %s\n", strerror(errno));
    if (archive >= 0) close(archive);
    if (box >= 0) close(box);
    if (tree >= 0) close(tree);
    if (source >= 0) close(source);
    if (module_root >= 0) close(module_root);
    json_free(&modules); buffer_destroy(&order); kernel_stage_end(&stage);
    return result ? 1 : 0;
}
