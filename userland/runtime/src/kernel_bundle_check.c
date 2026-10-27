/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_bundle_internal.h"
#include "bootloader_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief Accept the canonical installed-product alphabet and reject traversal.
 * @param name Borrowed relative product name.
 * @return Nonzero for nonempty, bounded canonical paths with no dot components.
 */
static int product_name(const char *name)
{
    if (!name || !*name || *name == '/' || strlen(name) >= BOOT_PATH_MAX) return 0;
    const char *start = name;
    for (const char *at = name;; ++at) {
        unsigned char c = (unsigned char)*at;
        if (!c || c == '/') {
            size_t size = (size_t)(at - start);
            if (!size || (size == 1 && *start == '.') || (size == 2 && !memcmp(start, "..", 2))) return 0;
            if (!c) return 1;
            start = at + 1;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("._+-", c))) return 0;
    }
}

/** @brief Find one checksum-list product without reopening a pathname.
 * @param bundle Borrowed complete or partially built snapshot.
 * @param name Borrowed full relative path.
 * @return Borrowed product or NULL if absent.
 */
static struct kernel_bundle_product *find_product(struct kernel_bundle_checked *bundle, const char *name)
{
    for (size_t i = 0; i < bundle->count; ++i) if (!strcmp(bundle->products[i].name, name)) return &bundle->products[i];
    return NULL;
}

/** @brief Reject checksum records outside this exact release's publication scope.
 * @param bundle Borrowed validated release identity.
 * @param name Canonical relative product name.
 * @return Nonzero only for module products, the platform policy or four boot members.
 */
static int allowed_product(const struct kernel_bundle_checked *bundle, const char *name)
{
    if (!strncmp(name, bundle->modules, strlen(bundle->modules))) return 1;
    if (strncmp(name, bundle->prefix, strlen(bundle->prefix))) return 0;
    name += strlen(bundle->prefix);
    return !strcmp(name, "kernel.sys") || !strcmp(name, "kerneldebug.sys") ||
           !strcmp(name, "initramfs.cpio") || !strcmp(name, "boot.env");
}

/** @brief Parse and verify a standard two-space SHA-256 inventory from this package.
 * @param bundle Mutable snapshot, which owns every successful product allocation.
 * @return Zero only when every unique canonical product matches its checksum; negative errno otherwise.
 */
static int checksum_list(struct kernel_bundle_checked *bundle)
{
    char path[128]; snprintf(path, sizeof(path), "%sSHA256SUMS", bundle->prefix);
    int fd = boot_open_file(bundle->root, path);
    if (fd < 0) return fd;
    FILE *stream = fdopen(fd, "r");
    if (!stream) { int error = -errno; close(fd); return error; }
    char *line = NULL; size_t capacity = 0; ssize_t length; int result = 0;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        if (length < 68 || memchr(line, 0, (size_t)length) || line[length - 1] != '\n' || line[64] != ' ' || line[65] != ' ') {
            result = -EINVAL; break;
        }
        line[--length] = 0;
        for (size_t i = 0; i < 64; ++i)
            if (!((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f'))) { result = -EINVAL; break; }
        const char *name = line + 66;
        if (result || !product_name(name) || !allowed_product(bundle, name) || find_product(bundle, name)) { result = -EINVAL; break; }
        int member = boot_open_file(bundle->root, name);
        if (member < 0) { result = member; break; }
        char actual[65]; result = kernel_bundle_hash_fd(member, actual); close(member);
        if (result || memcmp(line, actual, 64)) { result = result ? result : -EIO; break; }
        if (bundle->count == SIZE_MAX / sizeof(*bundle->products)) { result = -EOVERFLOW; break; }
        struct kernel_bundle_product *grown = realloc(bundle->products, (bundle->count + 1) * sizeof(*grown));
        if (!grown) { result = -ENOMEM; break; }
        bundle->products = grown;
        struct kernel_bundle_product *product = &grown[bundle->count];
        *product = (struct kernel_bundle_product){.name = strdup(name)};
        if (!product->name) { result = -ENOMEM; break; }
        memcpy(product->hash, actual, sizeof(product->hash)); ++bundle->count;
    }
    if (ferror(stream) && !result) result = -EIO;
    free(line); fclose(stream);
    return result;
}

/** @brief Check that all core boot and module metadata appear in the checksum inventory.
 * @param bundle Borrowed complete product snapshot.
 * @return Zero for complete matching kernel/archive/ABI, negative EINVAL otherwise.
 */
static int required_products(struct kernel_bundle_checked *bundle)
{
    const char *const boot[] = {"kernel.sys", "kerneldebug.sys", "initramfs.cpio", "boot.env"};
    const char *const modules[] = {"config", "module.abi", "Module.symvers", "System.map", "modules.order",
        "modules.builtin", "modules.builtin.modinfo", "modules.dep", "modules.alias", "modules.symbols",
        "modules-load.d/reliefnt-platform.conf"};
    char path[128];
    for (size_t i = 0; i < sizeof(boot) / sizeof(boot[0]); ++i) {
        snprintf(path, sizeof(path), "%s%s", bundle->prefix, boot[i]);
        struct kernel_bundle_product *product = find_product(bundle, path);
        if (!product) return -EINVAL;
        const char *expected = i == 0 ? bundle->identity.kernel_sha256 : i == 2 ? bundle->identity.initramfs_sha256 : NULL;
        if (expected && strcmp(product->hash, expected)) return -EINVAL;
    }
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        snprintf(path, sizeof(path), "%s%s", bundle->modules, modules[i]);
        if (!find_product(bundle, path)) return -EINVAL;
    }
    snprintf(path, sizeof(path), "%smodule.abi", bundle->modules);
    char abi[65]; int result = kernel_bundle_read_fixed(bundle->root, path, abi, sizeof(abi));
    return result ? result : memcmp(abi, bundle->identity.module_abi, 64) || abi[64] != '\n' ? -EINVAL : 0;
}

/** @brief Check the exact external module set and CONFIG_MODULES constraint.
 * @param bundle Mutable snapshot; marks ordered module products during validation.
 * @return Zero for complete unique modules.order membership, negative errno otherwise.
 */
static int module_inventory(struct kernel_bundle_checked *bundle)
{
    char path[BOOT_PATH_MAX]; snprintf(path, sizeof(path), "%smodules.order", bundle->modules);
    int fd = boot_open_file(bundle->root, path);
    if (fd < 0) return fd;
    FILE *stream = fdopen(fd, "r");
    if (!stream) { int result = -errno; close(fd); return result; }
    char *line = NULL; size_t capacity = 0, count = 0, ordered = 0; ssize_t length; int result = 0;
    for (size_t i = 0; i < bundle->count; ++i) {
        size_t size = strlen(bundle->products[i].name);
        if (size >= 3 && !strcmp(bundle->products[i].name + size - 3, ".ko")) ++count;
    }
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        if (length < 5 || memchr(line, 0, (size_t)length) || line[length - 1] != '\n') { result = -EINVAL; break; }
        line[--length] = 0;
        int full_size = snprintf(path, sizeof(path), "%s%s", bundle->modules, line);
        if (!product_name(line) || strcmp(line + length - 3, ".ko") || full_size < 0 || (size_t)full_size >= sizeof(path)) {
            result = -EINVAL; break;
        }
        struct kernel_bundle_product *product = find_product(bundle, path);
        if (!product || product->ordered) { result = -EINVAL; break; }
        product->ordered = 1; ++ordered;
    }
    if ((ferror(stream) || count != ordered) && !result) result = -EINVAL;
    free(line); line = NULL; capacity = 0; fclose(stream);
    if (result || !count) return result;
    snprintf(path, sizeof(path), "%sconfig", bundle->modules);
    fd = boot_open_file(bundle->root, path);
    if (fd < 0) return fd;
    stream = fdopen(fd, "r");
    if (!stream) { result = -errno; close(fd); return result; }
    unsigned enabled = 0;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        if (memchr(line, 0, (size_t)length)) { result = -EINVAL; break; }
        if (!strcmp(line, "CONFIG_MODULES=y\n")) ++enabled;
    }
    if (ferror(stream) || enabled != 1) result = -EINVAL;
    free(line); fclose(stream);
    return result;
}

int kernel_bundle_check_at(struct kernel_bundle_checked *bundle, int root, const char *release)
{
    if (!bundle || root < 0 || !release || !*release || strlen(release) >= sizeof(bundle->identity.release) ||
        !product_name(release) || strchr(release, '/')) return -EINVAL;
    bundle->root = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (bundle->root < 0) return -errno;
    snprintf(bundle->prefix, sizeof(bundle->prefix), "usr/lib/reliefos/kernel/%s/", release);
    snprintf(bundle->modules, sizeof(bundle->modules), "lib/modules/%s/", release);
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE]; char path[128];
    snprintf(path, sizeof(path), "%sboot.env", bundle->prefix);
    int result = kernel_bundle_read_fixed(bundle->root, path, record, sizeof(record));
    if (!result) result = reliefos_kernel_slot_decode(record, sizeof(record), &bundle->identity);
    if (!result && (bundle->identity.generation != 1 || strcmp(release, bundle->identity.release))) result = -EINVAL;
    if (!result) result = checksum_list(bundle);
    if (!result) result = required_products(bundle);
    if (!result) result = module_inventory(bundle);
    return result;
}

int kernel_bundle_check_open(struct kernel_bundle_checked *bundle, const char *root, const char *release)
{
    if (!bundle || !root) return -EINVAL;
    int fd = !strcmp(root, "/") ? open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : boot_open_directory(root);
    if (fd < 0) return !strcmp(root, "/") ? -errno : fd;
    int result = kernel_bundle_check_at(bundle, fd, release);
    close(fd);
    return result;
}

void kernel_bundle_check_close(struct kernel_bundle_checked *bundle)
{
    if (bundle->root >= 0) close(bundle->root);
    for (size_t i = 0; i < bundle->count; ++i) free(bundle->products[i].name);
    free(bundle->products);
    *bundle = (struct kernel_bundle_checked){.root = -1};
}

int reliefos_kernel_bundle_validate(const char *root, const char *release)
{
    struct kernel_bundle_checked bundle = {.root = -1};
    int result = kernel_bundle_check_open(&bundle, root, release);
    kernel_bundle_check_close(&bundle);
    return result;
}

int reliefos_kernel_bundle_describe(const char *root, const char *release, struct reliefos_kernel_slot *identity)
{
    if (!identity) return -EINVAL;
    struct kernel_bundle_checked bundle = {.root = -1};
    int result = kernel_bundle_check_open(&bundle, root, release);
    if (!result) *identity = bundle.identity;
    kernel_bundle_check_close(&bundle);
    return result;
}
