/* SPDX-License-Identifier: Apache-2.0 */
#include "userland/runtime/src/kernel_bundle_internal.h"
#include "userland/runtime/src/bootloader_internal.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>
#include <assert.h>

static unsigned operations;
ssize_t __real_write(int, const void *, size_t);
static void durable_write(int fd, const void *bytes, size_t size)
{
    const unsigned char *p = bytes;
    while (size) {
        ssize_t count = __real_write(fd, p, size);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0); p += count; size -= (size_t)count;
    }
}
/* A test-only durable inode image, captured exactly at successful fsync.
 * Unwrapped I/O keeps these oracle writes outside production fault counts. */
static void snapshot_durable_inode(int fd)
{
    const char *directory = getenv("RELIEFOS_TEST_DURABLE");
    if (!directory) return;
    struct stat status; assert(!fstat(fd, &status));
    char output[4096], descriptor[64];
    int size = snprintf(output, sizeof(output), "%s/%" PRIuMAX "-%" PRIuMAX,
                        directory, (uintmax_t)status.st_dev, (uintmax_t)status.st_ino);
    assert(size > 0 && (size_t)size < sizeof(output));
    int saved = open(output, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); assert(saved >= 0);
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    if (S_ISDIR(status.st_mode)) {
        durable_write(saved, "D\n", 2);
        DIR *stream = opendir(descriptor); assert(stream);
        struct dirent *entry;
        while ((entry = readdir(stream))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            assert(!strchr(entry->d_name, '\t') && !strchr(entry->d_name, '\n'));
            struct stat child; assert(!fstatat(dirfd(stream), entry->d_name, &child, AT_SYMLINK_NOFOLLOW));
            assert(S_ISREG(child.st_mode) || S_ISDIR(child.st_mode));
            char record[512];
            size = snprintf(record, sizeof(record), "%ju\t%ju-%ju\t%s\n", (uintmax_t)child.st_mode,
                            (uintmax_t)child.st_dev, (uintmax_t)child.st_ino, entry->d_name);
            assert(size > 0 && (size_t)size < sizeof(record)); durable_write(saved, record, (size_t)size);
        }
        assert(!closedir(stream));
    } else {
        assert(S_ISREG(status.st_mode)); durable_write(saved, "F\n", 2);
        int input = open(descriptor, O_RDONLY | O_CLOEXEC); assert(input >= 0);
        unsigned char bytes[65536]; ssize_t count;
        while ((count = read(input, bytes, sizeof(bytes)))) {
            if (count < 0 && errno == EINTR) continue;
            assert(count > 0); durable_write(saved, bytes, (size_t)count);
        }
        assert(!close(input));
    }
    assert(!close(saved));
}
static void trace_fd(char operation, int fd)
{
    const char *log = getenv("RELIEFOS_TEST_SYNC_LOG");
    if (!log) return;
    char descriptor[64], path[4096], line[4200];
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(descriptor, path, sizeof(path) - 1);
    if (size < 0) return;
    path[size] = 0;
    int length = snprintf(line, sizeof(line), "%c %s\n", operation, path);
    int output = open(log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (output >= 0) { (void)__real_write(output, line, (size_t)length); close(output); }
}
static int fail_operation(void)
{
    ++operations;
    const char *fail = getenv("RELIEFOS_TEST_FAIL");
    if (fail && strtoul(fail, NULL, 10) == operations) { errno = EIO; return 1; }
    return 0;
}
static void cut_operation(void)
{
    const char *cut = getenv("RELIEFOS_TEST_CUT");
    if (cut && strtoul(cut, NULL, 10) == operations) _exit(99);
}
ssize_t __wrap_write(int fd, const void *bytes, size_t size)
{
    if (fail_operation()) return -1;
    if (getenv("RELIEFOS_TEST_SHORT") && size > 1) size /= 2;
    ssize_t result = __real_write(fd, bytes, size); trace_fd('W', fd); cut_operation(); return result;
}
ssize_t __real_pwrite(int, const void *, size_t, off_t);
ssize_t __wrap_pwrite(int fd, const void *bytes, size_t size, off_t offset)
{
    if (fail_operation()) return -1;
    if (getenv("RELIEFOS_TEST_SHORT") && size > 1) size /= 2;
    ssize_t result = __real_pwrite(fd, bytes, size, offset); trace_fd('W', fd); cut_operation(); return result;
}
int __real_fsync(int);
int __wrap_fsync(int fd)
{
    if (fail_operation()) return -1;
    int result = __real_fsync(fd);
    if (!result) { trace_fd('S', fd); snapshot_durable_inode(fd); }
    cut_operation(); return result;
}
int __real_renameat(int, const char *, int, const char *);
int __wrap_renameat(int from, const char *old, int to, const char *new_name)
{
    if (fail_operation()) return -1;
    int result = __real_renameat(from, old, to, new_name); cut_operation(); return result;
}

int main(int argc, char **argv)
{
    if (argc == 4 && (!strcmp(argv[1], "retain") || !strcmp(argv[1], "restore"))) {
        int result = !strcmp(argv[1], "retain") ? reliefos_kernel_bundle_retain(argv[2], argv[3]) :
                                                 reliefos_kernel_bundle_restore(argv[2], argv[3]);
        printf("OPERATIONS=%u RESULT=%d\n", operations, result);
        return result ? 1 : 0;
    }
    if (argc == 4 && !strcmp(argv[1], "check")) {
        struct kernel_bundle_checked bundle = {.root = -1};
        int result = kernel_bundle_check_open(&bundle, argv[2], argv[3]);
        if (!result) printf("PASS checked %s products=%zu\n", bundle.identity.release, bundle.count);
        kernel_bundle_check_close(&bundle);
        return result ? 1 : 0;
    }
    if (argc == 3 && !strcmp(argv[1], "select")) {
        int root = boot_open_directory(argv[2]);
        if (root < 0) return 1;
        struct reliefos_kernel_slot selected; char name;
        int result = kernel_bundle_select(root, &selected, &name);
        close(root);
        if (!result) printf("%c %u %s\n", name, selected.generation, selected.release);
        return result ? 1 : 0;
    }
    if (argc == 5 && !strcmp(argv[1], "publish")) {
        int result = reliefos_kernel_bundle_publish(argv[2], argv[3], argv[4]);
        printf("OPERATIONS=%u RESULT=%d\n", operations, result);
        return result ? 1 : 0;
    }
    return 2;
}
