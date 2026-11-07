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

int kernel_slot_encode_command(int argc, char **argv)
{
    if (argc != 5) return 2;
    struct reliefos_kernel_slot slot = {0};
    char *end; errno = 0; unsigned long generation = strtoul(argv[0], &end, 10);
    if (errno || *end || !*argv[0] || argv[0][0] == '0' || strspn(argv[0], "0123456789") != strlen(argv[0]) ||
        generation > RELIEFOS_KERNEL_SLOT_GENERATION_MAX || strlen(argv[1]) >= sizeof(slot.release) ||
        strlen(argv[2]) != 64 || strlen(argv[3]) != 64 || strlen(argv[4]) != 64) return 1;
    slot.generation = (uint32_t)generation;
    strcpy(slot.release, argv[1]); strcpy(slot.module_abi, argv[2]);
    strcpy(slot.kernel_sha256, argv[3]); strcpy(slot.initramfs_sha256, argv[4]);
    unsigned char encoded[RELIEFOS_KERNEL_SLOT_SIZE];
    if (reliefos_kernel_slot_encode(&slot, encoded)) return 1;
    return fwrite(encoded, 1, sizeof(encoded), stdout) == sizeof(encoded) && !fflush(stdout) ? 0 : 1;
}

static int read_small(int root, const char *path, void *data, size_t length)
{
    int fd = kernel_bundle_open_member(root, path);
    if (fd < 0) return -1;
    struct stat status; size_t used = 0; int result = fstat(fd, &status) || status.st_size != (off_t)length;
    while (!result && used < length) {
        ssize_t count = read(fd, (unsigned char *)data + used, length - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = -1; break; }
        used += (size_t)count;
    }
    close(fd);
    return result ? -1 : 0;
}

static int member_hash(int root, const char *path, char digest[65])
{
    int fd = kernel_bundle_open_member(root, path);
    if (fd < 0) return -1;
    int result = kernel_tools_sha256_fd(fd, digest); close(fd);
    return result;
}

static int valid_slot(int root, char name, struct reliefos_kernel_slot *slot)
{
    char path[192], digest[65], checksum[400], expected[400], version[160];
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE];
    snprintf(path, sizeof(path), "reliefos/state/kernel-%c.env", name);
    if (read_small(root, path, record, sizeof(record)) ||
        reliefos_kernel_slot_decode(record, sizeof(record), slot) || member_hash(root, path, digest)) return -1;
    int length = snprintf(expected, sizeof(expected), "%s  kernel-%c.env\n", digest, name);
    snprintf(path, sizeof(path), "reliefos/state/kernel-%c.sha256", name);
    if (read_small(root, path, checksum, (size_t)length) || memcmp(checksum, expected, (size_t)length)) return -1;
    if (reliefos_kernel_boot_directory(slot, version, sizeof(version))) return -1;
    snprintf(path, sizeof(path), "%s/kernel.sys", version);
    if (member_hash(root, path, digest) || strcmp(digest, slot->kernel_sha256)) return -1;
    snprintf(path, sizeof(path), "%s/initramfs.cpio", version);
    if (member_hash(root, path, digest) || strcmp(digest, slot->initramfs_sha256)) return -1;
    snprintf(path, sizeof(path), "%s/boot.env", version);
    struct reliefos_kernel_slot template;
    if (read_small(root, path, record, sizeof(record)) || reliefos_kernel_slot_decode(record, sizeof(record), &template) ||
        template.generation != 1 || member_hash(root, path, digest)) return -1;
    template.generation = slot->generation;
    if (!reliefos_kernel_slot_equal(&template, slot)) return -1;
    length = snprintf(expected, sizeof(expected), "%s  kernel.sys\n%s  initramfs.cpio\n%s  boot.env\n",
                      slot->kernel_sha256, slot->initramfs_sha256, digest);
    snprintf(path, sizeof(path), "%s/SHA256SUMS", version);
    return read_small(root, path, checksum, (size_t)length) || memcmp(checksum, expected, (size_t)length) ? -1 : 0;
}

int kernel_slot_select_command(int argc, char **argv)
{
    if (argc != 1) return 2;
    int root = open(argv[0], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return 1;
    struct reliefos_kernel_slot slots[2];
    bool valid[] = {!valid_slot(root, 'A', &slots[0]), !valid_slot(root, 'B', &slots[1])};
    close(root);
    if ((!valid[0] && !valid[1]) || (valid[0] && valid[1] && slots[0].generation == slots[1].generation &&
        !reliefos_kernel_slot_equal(&slots[0], &slots[1]))) {
        fputs("kernel-tools: no unambiguous complete boot slot\n", stderr); return 1;
    }
    unsigned selected = !valid[0] || (valid[1] && slots[1].generation > slots[0].generation);
    const struct reliefos_kernel_slot *slot = &slots[selected];
    printf("{\"slot\":\"%c\",\"generation\":%u,\"release\":\"%s\",\"module_abi\":\"%s\","
           "\"kernel_sha256\":\"%s\",\"initramfs_sha256\":\"%s\"}\n", selected ? 'B' : 'A', slot->generation,
           slot->release, slot->module_abi, slot->kernel_sha256, slot->initramfs_sha256);
    return ferror(stdout) ? 1 : 0;
}

int kernel_slots_initialize(int root, const char *release, const char *abi,
                            const char *kernel_hash, const char *archive_hash)
{
    struct reliefos_kernel_slot slot = {.generation = 1};
    if (strlen(release) >= sizeof(slot.release) || strlen(abi) != 64 ||
        strlen(kernel_hash) != 64 || strlen(archive_hash) != 64) return -1;
    strcpy(slot.release, release); strcpy(slot.module_abi, abi);
    strcpy(slot.kernel_sha256, kernel_hash); strcpy(slot.initramfs_sha256, archive_hash);
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE];
    if (reliefos_kernel_slot_encode(&slot, record)) return -1;
    struct stat status;
    if (!fstatat(root, "reliefos/state", &status, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        fputs("kernel-tools: slot initialization requires a fresh private ESP tree\n", stderr); return -1;
    }
    for (char name = 'A'; name <= 'B'; ++name) {
        char path[96], hash[65], sums[80];
        snprintf(path, sizeof(path), "reliefos/state/kernel-%c.env", name);
        if (kernel_stage_write(root, path, record, sizeof(record), 0644) || member_hash(root, path, hash)) return -1;
        int length = snprintf(sums, sizeof(sums), "%s  kernel-%c.env\n", hash, name);
        snprintf(path, sizeof(path), "reliefos/state/kernel-%c.sha256", name);
        if (kernel_stage_write(root, path, sums, (size_t)length, 0644)) return -1;
    }
    return 0;
}

int kernel_slot_template(int root, const char *path, const char *release,
                         const char *abi, const char *kernel_hash, const char *archive_hash)
{
    struct reliefos_kernel_slot slot = {.generation = 1};
    if (strlen(release) >= sizeof(slot.release) || strlen(abi) != 64 ||
        strlen(kernel_hash) != 64 || strlen(archive_hash) != 64) return -1;
    strcpy(slot.release, release); strcpy(slot.module_abi, abi);
    strcpy(slot.kernel_sha256, kernel_hash); strcpy(slot.initramfs_sha256, archive_hash);
    unsigned char record[RELIEFOS_KERNEL_SLOT_SIZE];
    if (reliefos_kernel_slot_encode(&slot, record)) return -1;
    return kernel_stage_write(root, path, record, sizeof(record), 0644);
}
