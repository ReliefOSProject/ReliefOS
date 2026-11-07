/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_bundle_internal.h"
#include "bootloader_internal.h"
#include <mbedtls/sha256.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Convert one successful SHA-256 result to canonical lowercase text.
 * @param digest Borrowed 32-byte digest.
 * @param output Caller-owned 65-byte text buffer.
 */
static void hexadecimal(const unsigned char digest[32], char output[65])
{
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) { output[2 * i] = alphabet[digest[i] >> 4]; output[2 * i + 1] = alphabet[digest[i] & 15]; }
    output[64] = 0;
}

int kernel_bundle_hash_bytes(const void *bytes, size_t size, char output[65])
{
    if ((!bytes && size) || !output) return -EINVAL;
    unsigned char digest[32];
    int result = mbedtls_sha256(bytes, size, digest, 0);
    if (result) return -EIO;
    hexadecimal(digest, output);
    return 0;
}

int kernel_bundle_hash_fd(int fd, char output[65])
{
    struct stat status;
    if (!output) return -EINVAL;
    if (fstat(fd, &status)) return -errno;
    if (!S_ISREG(status.st_mode)) return -EINVAL;
    if (lseek(fd, 0, SEEK_SET) < 0) return -errno;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    int result = mbedtls_sha256_starts(&context, 0) ? -EIO : 0;
    unsigned char buffer[65536], digest[32];
    while (!result) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -errno; break; }
        if (!count) break;
        if (mbedtls_sha256_update(&context, buffer, (size_t)count)) result = -EIO;
    }
    if (!result && mbedtls_sha256_finish(&context, digest)) result = -EIO;
    mbedtls_sha256_free(&context);
    if (!result) hexadecimal(digest, output);
    return result;
}

int kernel_bundle_read_fixed(int root, const char *name, void *output, size_t size)
{
    if (!output && size) return -EINVAL;
    int fd = boot_open_file(root, name);
    if (fd < 0) return fd;
    struct stat status; size_t used = 0;
    int result = fstat(fd, &status) ? -errno : status.st_size < 0 || (uint64_t)status.st_size != size ? -EINVAL : 0;
    while (!result && used < size) {
        ssize_t count = read(fd, (unsigned char *)output + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = count ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    close(fd);
    return result;
}
