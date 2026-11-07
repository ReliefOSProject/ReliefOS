/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefos/kernel_bundle.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Validate a bounded SHA-256 C string without locale conversion.
 * @param text Borrowed 65-byte array, including its terminator.
 * @return Nonzero only for 64 lowercase hexadecimal digits and one NUL.
 */
static int digest_valid(const char text[65])
{
    for (size_t i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
    return !text[64];
}

/** @brief Validate scalar identity and bind the release suffix to its module ABI.
 * @param slot Borrowed fixed-size identity, nullable.
 * @return Nonzero only for a complete, canonical identity suitable for GRUB.
 */
static int identity_valid(const struct reliefos_kernel_slot *slot)
{
    if (!slot || !slot->generation || slot->generation > RELIEFOS_KERNEL_SLOT_GENERATION_MAX ||
        !digest_valid(slot->module_abi) || !digest_valid(slot->kernel_sha256) || !digest_valid(slot->initramfs_sha256)) return 0;
    const char *end = memchr(slot->release, 0, sizeof(slot->release));
    if (!end || end - slot->release < 12) return 0;
    size_t length = (size_t)(end - slot->release);
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)slot->release[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i && strchr("._+-", c)))) return 0;
    }
    return !memcmp(slot->release + length - 12, slot->module_abi, 12);
}

int reliefos_kernel_slot_encode(const struct reliefos_kernel_slot *slot,
                                unsigned char output[RELIEFOS_KERNEL_SLOT_SIZE])
{
    if (!output || !identity_valid(slot)) return -EINVAL;
    char text[RELIEFOS_KERNEL_SLOT_SIZE];
    int length = snprintf(text, sizeof(text), "# GRUB Environment Block\n"
        "reliefos_generation=%u\nreliefos_release=%s\nreliefos_module_abi=%s\n"
        "reliefos_kernel_sha256=%s\nreliefos_initramfs_sha256=%s\n", slot->generation,
        slot->release, slot->module_abi, slot->kernel_sha256, slot->initramfs_sha256);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EINVAL;
    memset(output, '#', RELIEFOS_KERNEL_SLOT_SIZE);
    memcpy(output, text, (size_t)length);
    return 0;
}

/** @brief Extract one exact, bounded line following an expected variable prefix.
 * @param cursor Borrowed cursor, advanced past the required newline on success.
 * @param end Exclusive bound of the exact record.
 * @param prefix Required constant variable name and equal sign.
 * @param value Caller-owned output string buffer.
 * @param capacity Output capacity including the terminator.
 * @return Zero on success, negative EINVAL on missing prefix/newline or overflow.
 */
static int field(const unsigned char **cursor, const unsigned char *end,
                  const char *prefix, char *value, size_t capacity)
{
    size_t prefix_size = strlen(prefix);
    if ((size_t)(end - *cursor) < prefix_size || memcmp(*cursor, prefix, prefix_size)) return -EINVAL;
    const unsigned char *start = *cursor + prefix_size;
    const unsigned char *newline = memchr(start, '\n', (size_t)(end - start));
    if (!newline || (size_t)(newline - start) >= capacity || memchr(start, 0, (size_t)(newline - start))) return -EINVAL;
    memcpy(value, start, (size_t)(newline - start)); value[newline - start] = 0;
    *cursor = newline + 1;
    return 0;
}

int reliefos_kernel_slot_decode(const void *input, size_t length, struct reliefos_kernel_slot *slot)
{
    static const char header[] = "# GRUB Environment Block\n";
    if (!input || !slot || length != RELIEFOS_KERNEL_SLOT_SIZE || memcmp(input, header, sizeof(header) - 1)) return -EINVAL;
    const unsigned char *cursor = (const unsigned char *)input + sizeof(header) - 1;
    const unsigned char *end = (const unsigned char *)input + length;
    struct reliefos_kernel_slot parsed = {0}; char generation[16];
    if (field(&cursor, end, "reliefos_generation=", generation, sizeof(generation)) ||
        field(&cursor, end, "reliefos_release=", parsed.release, sizeof(parsed.release)) ||
        field(&cursor, end, "reliefos_module_abi=", parsed.module_abi, sizeof(parsed.module_abi)) ||
        field(&cursor, end, "reliefos_kernel_sha256=", parsed.kernel_sha256, sizeof(parsed.kernel_sha256)) ||
        field(&cursor, end, "reliefos_initramfs_sha256=", parsed.initramfs_sha256, sizeof(parsed.initramfs_sha256))) return -EINVAL;
    if (!*generation || generation[0] == '0') return -EINVAL;
    uint32_t number = 0;
    for (const char *at = generation; *at; ++at) {
        if (*at < '0' || *at > '9' || number > (RELIEFOS_KERNEL_SLOT_GENERATION_MAX - (unsigned)(*at - '0')) / 10) return -EINVAL;
        number = number * 10 + (unsigned)(*at - '0');
    }
    parsed.generation = number;
    unsigned char canonical[RELIEFOS_KERNEL_SLOT_SIZE];
    if (reliefos_kernel_slot_encode(&parsed, canonical) || memcmp(input, canonical, length)) return -EINVAL;
    *slot = parsed;
    return 0;
}

int reliefos_kernel_slot_equal(const struct reliefos_kernel_slot *a, const struct reliefos_kernel_slot *b)
{
    return a && b && a->generation == b->generation && !strcmp(a->release, b->release) &&
        !strcmp(a->module_abi, b->module_abi) && !strcmp(a->kernel_sha256, b->kernel_sha256) &&
        !strcmp(a->initramfs_sha256, b->initramfs_sha256);
}

int reliefos_kernel_boot_directory(const struct reliefos_kernel_slot *slot, char *output, size_t capacity)
{
    if (!output || !identity_valid(slot)) return -EINVAL;
    int size = snprintf(output, capacity, "reliefos/versions/%s/%s", slot->release, slot->initramfs_sha256);
    return size < 0 || (size_t)size >= capacity ? -ENAMETOOLONG : 0;
}
