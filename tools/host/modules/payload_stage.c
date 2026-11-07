/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include "userland/runtime/include/reliefos/kernel_bundle.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct boot_pair {
    json_value document;
    const char *release, *abi, *kernel_hash, *archive_hash;
    int archive, kernel;
};

static void pair_close(struct boot_pair *pair)
{
    if (pair->archive >= 0) close(pair->archive);
    if (pair->kernel >= 0) close(pair->kernel);
    json_free(&pair->document);
}

static int pair_open(struct boot_pair *pair, const char *kernel, const char *archive)
{
    char *metadata = malloc(strlen(archive) + sizeof(".json"));
    if (!metadata) return -1;
    sprintf(metadata, "%s.json", archive);
    int result = kernel_stage_document_path(metadata, &pair->document);
    free(metadata);
    long schema;
    if (result || kernel_stage_identity(&pair->document, &pair->release, &pair->abi, &pair->kernel_hash) ||
        json_int(json_member(&pair->document, "schema"), &schema) || schema != 1) {
        fputs("kernel-tools: kernel/modules/initramfs identity mismatch\n", stderr); return -1;
    }
    pair->archive_hash = json_text(json_member(&pair->document, "initramfs_sha256"));
    if (!kernel_tools_digest_text(pair->archive_hash)) { errno = EINVAL; return -1; }
    pair->archive = kernel_stage_input(archive);
    char actual[65];
    if (pair->archive < 0 || kernel_tools_sha256_fd(pair->archive, actual) || strcmp(actual, pair->archive_hash) ||
        lseek(pair->archive, 0, SEEK_SET) < 0) {
        fputs("kernel-tools: initramfs checksum mismatch\n", stderr); return -1;
    }
    if (kernel) {
        pair->kernel = kernel_stage_input(kernel);
        if (pair->kernel < 0 || kernel_tools_sha256_fd(pair->kernel, actual) || strcmp(actual, pair->kernel_hash) ||
            lseek(pair->kernel, 0, SEEK_SET) < 0) {
            fputs("kernel-tools: kernel checksum mismatch\n", stderr); return -1;
        }
    }
    return 0;
}

static int payload_manifest(struct kernel_stage *stage, const struct kernel_bundle *bundle,
                             const char *prefix)
{
    struct kernel_stage_inventory files = {0};
    if (kernel_stage_inventory(stage->root_fd, &files)) { kernel_stage_inventory_free(&files); return -1; }
    /* The standard checksum list is the bounded runtime consumer's input.
     * JSON additionally covers that list, while neither format hashes itself. */
    char *sums = NULL; size_t sums_size = 0;
    FILE *sum_stream = open_memstream(&sums, &sums_size);
    if (!sum_stream) { kernel_stage_inventory_free(&files); return -1; }
    for (size_t i = 0; i < files.count; ++i) fprintf(sum_stream, "%s  %s\n", files.files[i].sha256, files.files[i].name);
    int sum_error = ferror(sum_stream);
    if (fclose(sum_stream)) sum_error = 1;
    char sum_path[128]; snprintf(sum_path, sizeof(sum_path), "%sSHA256SUMS", prefix);
    if (!sum_error) sum_error = kernel_stage_write(stage->root_fd, sum_path, sums, sums_size, 0644);
    free(sums); kernel_stage_inventory_free(&files);
    /* The signed APK's current pointer is outer package metadata. It is not a
     * per-release recovery product: restoring an old tree must never replace
     * the new package's pointer. The outer JSON inventory still hashes it. */
    char release_line[34]; int release_size = snprintf(release_line, sizeof(release_line), "%s\n", bundle->release);
    if (!sum_error) sum_error = kernel_stage_write(stage->root_fd, "usr/lib/reliefos/kernel/current-release", release_line, (size_t)release_size, 0644);
    if (sum_error || kernel_stage_inventory(stage->root_fd, &files)) { kernel_stage_inventory_free(&files); return -1; }
    char *text = NULL; size_t size = 0;
    FILE *stream = open_memstream(&text, &size);
    if (!stream) { kernel_stage_inventory_free(&files); return -1; }
    fprintf(stream, "{\"schema\":2,\"release\":\"%s\",\"module_abi\":\"%s\",\"kernel\":\"%skernel.sys\","
                    "\"initramfs\":\"%sinitramfs.cpio\",\"module_directory\":\"lib/modules/%s\",\"files\":",
            bundle->release, bundle->abi, prefix, prefix, bundle->release);
    int result = kernel_stage_files_json(stream, &files);
    fputs("}\n", stream);
    if (ferror(stream)) result = -1;
    if (fclose(stream)) result = -1;
    char member[128]; snprintf(member, sizeof(member), "%sbundle.json", prefix);
    if (!result) result = kernel_stage_write(stage->root_fd, member, text, size, 0644);
    if (!result) result = kernel_stage_write(stage->root_fd, "kernel-payload.json", text, size, 0644);
    free(text); kernel_stage_inventory_free(&files);
    return result;
}

int kernel_payload_stage(int argc, char **argv)
{
    if (argc != 4) return 2;
    struct kernel_bundle bundle = {.root_fd = -1};
    struct kernel_stage stage = {.root_fd = -1, .parent_fd = -1};
    struct boot_pair pair = {.archive = -1, .kernel = -1};
    json_value modules = {0}; int module_root = -1, result = -1;
    if (kernel_bundle_open(&bundle, argv[0], 0, NULL)) { fprintf(stderr, "kernel-tools: %s\n", bundle.error); goto out; }
    module_root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (module_root < 0 || kernel_stage_document(module_root, "modules-stage.json", &modules)) goto out;
    const char *release, *abi, *kernel_hash;
    long schema;
    if (kernel_stage_identity(&modules, &release, &abi, &kernel_hash) ||
        json_int(json_member(&modules, "schema"), &schema) || schema != 1 || strcmp(release, bundle.release) ||
        strcmp(abi, bundle.abi) || strcmp(kernel_hash, kernel_bundle_find(&bundle, "kernel.sys")->sha256)) {
        fputs("kernel-tools: kernel/modules/initramfs identity mismatch\n", stderr); goto out;
    }
    if (pair_open(&pair, NULL, argv[2])) goto out;
    if (strcmp(pair.release, release) || strcmp(pair.abi, abi) || strcmp(pair.kernel_hash, kernel_hash)) {
        fputs("kernel-tools: kernel/modules/initramfs identity mismatch\n", stderr); goto out;
    }
    const json_value *files = json_member(&modules, "files");
    if (kernel_stage_verify(module_root, files) || kernel_stage_begin(&stage, argv[3])) goto out;
    for (size_t i = 0; i < files->child_count; ++i) {
        const char *name = files->keys[i], *hash = json_text(json_member(&files->children[i], "sha256"));
        if (strncmp(name, bundle.prefix, strlen(bundle.prefix))) {
            errno = EINVAL; goto out;
        }
        if (kernel_stage_copy(module_root, name, stage.root_fd, name, hash, 0644)) goto out;
    }
    char prefix[96], member[128]; snprintf(prefix, sizeof(prefix), "usr/lib/reliefos/kernel/%s/", release);
    const char *names[] = {"kernel.sys", "kerneldebug.sys"};
    for (size_t i = 0; i < 2; ++i) {
        snprintf(member, sizeof(member), "%s%s", prefix, names[i]);
        if (kernel_stage_copy(bundle.root_fd, names[i], stage.root_fd, member, kernel_bundle_find(&bundle, names[i])->sha256, 0644)) goto out;
    }
    snprintf(member, sizeof(member), "%sinitramfs.cpio", prefix);
    if (kernel_stage_copy_fd(pair.archive, stage.root_fd, member, pair.archive_hash, 0644)) goto out;
    snprintf(member, sizeof(member), "%sboot.env", prefix);
    if (kernel_slot_template(stage.root_fd, member, release, abi, kernel_hash, pair.archive_hash) ||
        payload_manifest(&stage, &bundle, prefix) || kernel_stage_publish(&stage, "kernel-payload.json")) goto out;
    result = 0;
out:
    if (result) fprintf(stderr, "kernel-tools: kernel payload stage failed: %s\n", strerror(errno));
    if (module_root >= 0) close(module_root);
    json_free(&modules); pair_close(&pair); kernel_stage_end(&stage); kernel_bundle_close(&bundle);
    return result ? 1 : 0;
}

int kernel_boot_stage(int argc, char **argv)
{
    if (argc != 3) return 2;
    struct boot_pair pair = {.archive = -1, .kernel = -1};
    int root = -1, result = -1;
    if (pair_open(&pair, argv[0], argv[1])) goto out;
    root = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat target, filesystem_root;
    if (root < 0 || fstat(root, &target) || stat("/", &filesystem_root) ||
        (target.st_ino == filesystem_root.st_ino && target.st_dev == filesystem_root.st_dev)) goto out;
    struct reliefos_kernel_slot identity = {.generation = 1};
    snprintf(identity.release, sizeof(identity.release), "%s", pair.release);
    snprintf(identity.module_abi, sizeof(identity.module_abi), "%s", pair.abi);
    snprintf(identity.kernel_sha256, sizeof(identity.kernel_sha256), "%s", pair.kernel_hash);
    snprintf(identity.initramfs_sha256, sizeof(identity.initramfs_sha256), "%s", pair.archive_hash);
    char version[160];
    if (reliefos_kernel_boot_directory(&identity, version, sizeof(version))) goto out;
    const char *directories[] = {"reliefos", "leonos", version};
    for (size_t i = 0; i < 3; ++i) {
        char member[192];
        snprintf(member, sizeof(member), "%s/kernel.sys", directories[i]);
        if (lseek(pair.kernel, 0, SEEK_SET) < 0 || kernel_stage_copy_fd(pair.kernel, root, member, pair.kernel_hash, 0644)) goto out;
        snprintf(member, sizeof(member), "%s/initramfs.cpio", directories[i]);
        if (lseek(pair.archive, 0, SEEK_SET) < 0 || kernel_stage_copy_fd(pair.archive, root, member, pair.archive_hash, 0644)) goto out;
    }
    char member[192], sums[300], template_hash[65];
    snprintf(member, sizeof(member), "%s/boot.env", version);
    if (kernel_slot_template(root, member, pair.release, pair.abi, pair.kernel_hash, pair.archive_hash)) goto out;
    int template = kernel_bundle_open_member(root, member);
    int bad_template = template < 0 || kernel_tools_sha256_fd(template, template_hash);
    if (template >= 0) close(template);
    if (bad_template) goto out;
    int size = snprintf(sums, sizeof(sums), "%s  kernel.sys\n%s  initramfs.cpio\n%s  boot.env\n",
                        pair.kernel_hash, pair.archive_hash, template_hash);
    snprintf(member, sizeof(member), "%s/SHA256SUMS", version);
    if (kernel_stage_write(root, member, sums, (size_t)size, 0644)) goto out;
    char *text = NULL; size_t length = 0;
    FILE *stream = open_memstream(&text, &length);
    if (!stream) goto out;
    int failed = kernel_stage_json(stream, &pair.document);
    fputc('\n', stream);
    if (ferror(stream)) failed = -1;
    if (fclose(stream)) failed = -1;
    snprintf(member, sizeof(member), "%s/boot.json", version);
    if (!failed) failed = kernel_stage_write(root, member, text, length, 0644);
    free(text);
    if (failed || kernel_slots_initialize(root, pair.release, pair.abi, pair.kernel_hash, pair.archive_hash) || fsync(root)) goto out;
    result = 0;
out:
    if (result) fprintf(stderr, "kernel-tools: boot pair stage failed: %s\n", strerror(errno));
    if (root >= 0) close(root);
    pair_close(&pair);
    return result ? 1 : 0;
}
