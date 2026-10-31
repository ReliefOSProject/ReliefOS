/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "tools/host/modules/stage.h"
#include "userland/runtime/include/reliefos/kernel_bundle.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** @brief Accept the public three-part image version without executable syntax. */
static int image_version(const char *text)
{
    if (!text || !*text || strlen(text) > 31) return 0;
    unsigned groups = 1, digits = 0;
    for (; *text; ++text) {
        if (*text >= '0' && *text <= '9') ++digits;
        else if (*text == '.' && digits) { ++groups; digits = 0; }
        else return 0;
    }
    return groups == 3 && digits;
}

/** @brief Hash a regular explicit input, retaining no pathname-derived output. */
static int input_hash(const char *path, char output[65])
{
    int fd = kernel_stage_input(path);
    if (fd < 0) return -1;
    int result = kernel_tools_sha256_fd(fd, output); close(fd);
    return result;
}

/** @brief Parse only the four scalar lines emitted by the actual runtime verifier. */
static int inspect_identity(const char *program, const char *root, const char *release,
                            struct reliefos_kernel_slot *identity)
{
    char *arguments[] = {(char *)program, "--inspect", (char *)root, (char *)release, NULL};
    struct byte_buffer bytes = {0}; int result = kernel_stage_run(arguments, &bytes);
    const char *names[] = {"kernel_release=", "module_abi=", "kernel_sha256=", "initramfs_sha256="};
    char *fields[] = {identity->release, identity->module_abi, identity->kernel_sha256, identity->initramfs_sha256};
    const size_t capacities[] = {sizeof(identity->release), 65, 65, 65};
    size_t offset = 0;
    for (size_t i = 0; !result && i < 4; ++i) {
        size_t prefix = strlen(names[i]);
        if (bytes.len - offset < prefix || memcmp(bytes.data + offset, names[i], prefix)) { result = -1; break; }
        offset += prefix;
        unsigned char *end = memchr(bytes.data + offset, '\n', bytes.len - offset);
        if (!end || (size_t)(end - bytes.data - offset) >= capacities[i]) { result = -1; break; }
        size_t size = (size_t)(end - bytes.data - offset);
        if (memchr(bytes.data + offset, 0, size)) { result = -1; break; }
        memcpy(fields[i], bytes.data + offset, size); fields[i][size] = 0;
        offset += size + 1;
    }
    if (!result && offset != bytes.len) result = -1;
    identity->generation = 1;
    unsigned char encoded[RELIEFOS_KERNEL_SLOT_SIZE];
    if (!result) result = reliefos_kernel_slot_encode(identity, encoded);
    buffer_destroy(&bytes);
    return result;
}

/** @brief Emit protocol 4 only for one complete runtime-checked signed-package cohort.
 * The producer's mkndx step verifies the APK before invoking this command. The
 * frozen index and archive hashes bind update preflight to the same cohort.
 */
int kernel_release_info(int argc, char **argv)
{
    if (argc != 6 || !image_version(argv[4]) || argv[5][0] != '/') return 2;
    const char *package = strrchr(argv[2], '/'); package = package ? package + 1 : argv[2];
    static const char prefix[] = "reliefos-kernel-";
    size_t length = strlen(package), start = sizeof(prefix) - 1;
    char version[192];
    if (length <= start + 4 || strncmp(package, prefix, start) || strcmp(package + length - 4, ".apk") ||
        length - start - 4 >= sizeof(version)) return 1;
    memcpy(version, package + start, length - start - 4); version[length - start - 4] = 0;
    if (version[0] < '0' || version[0] > '9' || strspn(version, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") != strlen(version)) return 1;
    int root = open(argv[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    json_value document = {0}; int result = -1;
    if (root < 0 || kernel_stage_document(root, "kernel-payload.json", &document)) goto out;
    const char *release = json_text(json_member(&document, "release"));
    const char *abi = json_text(json_member(&document, "module_abi"));
    long schema;
    if (!release || !abi || json_int(json_member(&document, "schema"), &schema) || schema != 2 ||
        kernel_stage_verify(root, json_member(&document, "files"))) goto out;
    struct reliefos_kernel_slot identity = {0};
    if (inspect_identity(argv[5], argv[0], release, &identity) || strcmp(release, identity.release) || strcmp(abi, identity.module_abi)) goto out;
    char kernel[65], archive[65], index[65], manifest[65];
    if (input_hash(argv[1], kernel) || strcmp(kernel, identity.kernel_sha256) ||
        input_hash(argv[2], archive) || input_hash(argv[3], index)) goto out;
    int fd = kernel_bundle_open_member(root, "kernel-payload.json");
    if (fd < 0) goto out;
    int bad = kernel_tools_sha256_fd(fd, manifest); close(fd);
    if (bad) goto out;
    if (printf("format_version=4\nimage_version=%s\nversion=%s\nkernel_release=%s\nmodule_abi=%s\n"
               "kernel_sha256=%s\ninitramfs_sha256=%s\npackage=reliefos-kernel\npackage_version=%s\n"
               "package_file=%s\npackage_sha256=%s\nrepository_sha256=%s\nbundle_manifest_sha256=%s\n"
               "module_directory=lib/modules/%s\n", argv[4], argv[4], identity.release, identity.module_abi,
               identity.kernel_sha256, identity.initramfs_sha256, version, package, archive, index, manifest, identity.release) < 0 || fflush(stdout)) goto out;
    result = 0;
out:
    if (root >= 0) close(root);
    json_free(&document);
    if (result) fputs("kernel-tools: incomplete or mismatched kernel release cohort\n", stderr);
    return result ? 1 : 0;
}
