/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static int scalar(const char *value, size_t limit)
{
    size_t size = strlen(value);
    return size && size <= limit && strcmp(value, ".") && strcmp(value, "..") &&
        strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") == size;
}

static int image_version(const char *value)
{
    unsigned dots = 0, digits = 0;
    if (!value || strlen(value) > 31) return 0;
    for (; *value; ++value) {
        if (*value >= '0' && *value <= '9') ++digits;
        else if (*value == '.' && digits && dots < 2) { ++dots; digits = 0; }
        else return 0;
    }
    return dots == 2 && digits;
}

static int hash_equal(const char *path, const char *expected)
{
    char actual[65]; int result = update_hash(path, actual);
    return result ? result : strcmp(actual, expected) ? -EBADMSG : 0;
}

/* The transport manifest is a bounded description, never executable input.
 * Authenticity comes from the existing trusted signed index and signed APK. */
int update_kernel_check(struct update_context *context, int installed)
{
    const char *keys[] = {"format_version", "image_version", "version", "kernel_release", "module_abi",
        "kernel_sha256", "initramfs_sha256", "package", "package_version", "package_file", "package_sha256",
        "repository_sha256", "bundle_manifest_sha256", "module_directory"};
    char *values[14] = {0}, *bytes = NULL, path[PATH_MAX]; size_t size;
    int result = update_path(path, context->plan, "release.txt");
    if (!result && !installed) {
        char url[2048], address[2112]; result = update_rpr_url(context, url);
        int count = result ? -1 : snprintf(address, sizeof(address), "%s/kernel/release.txt", url);
        if (!result && (count < 0 || (size_t)count >= sizeof(address))) result = -ENAMETOOLONG;
        char *arguments[] = {context->fetch, address, path, "4096", NULL};
        if (!result) result = update_run(context, arguments, NULL, 0);
    }
    if (!result) result = update_read(path, &bytes, &size, 4096);
    if (result) return result;
    for (size_t i = 0; i < size; ++i) {
        unsigned char value = (unsigned char)bytes[i];
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || strchr("_.+=/\n-", value)) || !value) { result = -EINVAL; break; }
    }
    size_t fields = 0; char *save = NULL;
    for (char *line = result ? NULL : strtok_r(bytes, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *separator = strchr(line, '=');
        if (!separator || !separator[1] || strchr(separator + 1, '=')) { result = -EINVAL; break; }
        *separator++ = 0; size_t index;
        for (index = 0; index < 14 && strcmp(keys[index], line); ++index) {}
        if (index == 14 || values[index]) { result = -EINVAL; break; }
        values[index] = separator; ++fields;
    }
    if (!result && fields != 14) result = -EINVAL;
    if (!result && (strcmp(values[0], "4") || strcmp(values[1], values[2]) || !image_version(values[1]) ||
                   !scalar(values[3], 31) || strcmp(values[7], "reliefos-kernel") || !scalar(values[8], 128) ||
                   values[8][0] < '0' || values[8][0] > '9')) result = -EINVAL;
    const size_t hashes[] = {4, 5, 6, 10, 11, 12};
    for (size_t i = 0; !result && i < sizeof(hashes)/sizeof(hashes[0]); ++i)
        if (!update_token(values[hashes[i]])) result = -EINVAL;
    char expected[256];
    if (!result) {
        snprintf(expected, sizeof(expected), "reliefos-kernel-%s.apk", values[8]);
        if (strcmp(expected, values[9])) result = -EINVAL;
        snprintf(expected, sizeof(expected), "lib/modules/%s", values[3]);
        if (strcmp(expected, values[13])) result = -EINVAL;
    }
    if (!result && !installed) {
        char preview[PATH_MAX], *text; size_t length;
        result = update_path(preview, context->plan, "preview");
        if (!result) result = update_read(preview, &text, &length, UPDATE_OUTPUT_MAX);
        if (!result) {
            char upgrade[180], install[180];
            snprintf(upgrade, sizeof(upgrade), " -> %s)", values[8]);
            snprintf(install, sizeof(install), "Installing reliefos-kernel (%s)", values[8]);
            const char *change = strstr(text, "Upgrading reliefos-kernel (");
            if (!change) change = strstr(text, "Downgrading reliefos-kernel (");
            const char *end = change ? strchr(change, '\n') : NULL;
            const char *version = change ? strstr(change, upgrade) : NULL;
            if (!strstr(text, install) && !(change && version && (!end || version < end))) result = -ESTALE;
            free(text);
        }
        /* Find the exact RPR index among APK's frozen, verified cache entries. */
        char indexes[PATH_MAX]; if (!result) result = update_path(indexes, context->plan, "indexes");
        DIR *directory = result ? NULL : opendir(indexes); int matched = 0;
        if (!result && !directory) result = -errno;
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory))) {
                if (strncmp(entry->d_name, "APKINDEX.", 9)) continue;
                char candidate[PATH_MAX], actual[65];
                if (!update_path(candidate, indexes, entry->d_name) && !update_hash(candidate, actual) && !strcmp(actual, values[11])) matched = 1;
            }
            closedir(directory);
        }
        if (!result && !matched) result = -ESTALE;
        char archives[PATH_MAX], extraction[PATH_MAX], package[PATH_MAX], solver[PATH_MAX];
        if (!result) result = update_path(archives, context->plan, "kernel-archive");
        int fd = result ? -1 : update_directory(archives, 1); if (fd < 0 && !result) result = fd; if (fd >= 0) close(fd);
        if (!result) result = update_path(extraction, context->plan, "kernel-extracted");
        fd = result ? -1 : update_directory(extraction, 1); if (fd < 0 && !result) result = fd; if (fd >= 0) close(fd);
        if (!result) result = update_path(solver, context->plan, "snapshot");
        if (!result) result = update_path(package, archives, values[9]);
        /* Consume the exact signed solver cache, rather than solving/fetching
         * the kernel a second time. Native cache names carry an eight-digit
         * index checksum; the release manifest binds the complete archive. */
        char prefix[180]; int prefix_size = snprintf(prefix, sizeof(prefix), "reliefos-kernel-%s.", values[8]);
        directory = result ? NULL : opendir(context->cache); matched = 0;
        if (!result && !directory) result = -errno;
        if (directory) {
            struct dirent *entry;
            while (!result && (entry = readdir(directory))) {
                const char *name = entry->d_name;
                if (strncmp(name, prefix, (size_t)prefix_size)) continue;
                const char *checksum = name + prefix_size;
                if (strlen(checksum) != 12 || strspn(checksum, "0123456789abcdef") != 8 || strcmp(checksum + 8, ".apk")) continue;
                char candidate[PATH_MAX];
                result = update_path(candidate, context->cache, name);
                if (!result) result = hash_equal(candidate, values[10]);
                if (!result && matched++) result = -EEXIST;
                if (!result) result = update_copy(candidate, package, 0600);
            }
            closedir(directory);
        }
        if (!result && !matched) result = -ENOENT;
        if (!result) result = hash_equal(package, values[10]);
        const char *extract[] = {"extract", "--no-chown", "--destination", extraction, package, NULL};
        if (!result) result = update_apk(context, solver, 0, NULL, extract);
        if (!result) {
            char member[160]; snprintf(member, sizeof(member), "usr/lib/reliefos/kernel/%s/bundle.json", values[3]);
            result = update_path(path, extraction, member);
            if (!result) result = hash_equal(path, values[12]);
        }
        if (!result) result = update_path(path, context->plan, "kernel-identity");
        char *inspect[] = {context->bundle, "--inspect", extraction, values[3], NULL};
        if (!result) result = update_run(context, inspect, path, 0);
    } else if (!result) {
        result = update_path(path, context->plan, "installed-kernel-identity");
        char *inspect[] = {context->bundle, "--inspect", context->root, values[3], NULL};
        if (!result) result = update_run(context, inspect, path, 1);
    }
    if (!result) {
        char identity[512], *actual; size_t length;
        int count = snprintf(identity, sizeof(identity), "kernel_release=%s\nmodule_abi=%s\nkernel_sha256=%s\ninitramfs_sha256=%s\n",
                             values[3], values[4], values[5], values[6]);
        result = update_read(path, &actual, &length, sizeof(identity));
        if (!result) {
            if (count < 0 || (size_t)count != length || memcmp(identity, actual, length)) result = -EBADMSG;
            free(actual);
        }
    }
    free(bytes); return result;
}

static int record_field(const char *text, const char *key, char *output, size_t capacity)
{
    size_t key_size = strlen(key); int found = 0;
    const char *at = text;
    while (*at) {
        const char *end = strchr(at, '\n');
        if (!end) return -EINVAL;
        if ((size_t)(end-at) > key_size && !memcmp(at, key, key_size) && at[key_size] == '=') {
            size_t size = (size_t)(end-at)-key_size-1;
            if (found++ || !size || size >= capacity) return -EINVAL;
            memcpy(output, at+key_size+1, size); output[size] = 0;
        }
        at = end+1;
    }
    return found == 1 ? 0 : -EINVAL;
}

struct boot_identity {
    char slot[2], generation[16], release[32], initramfs[65];
};

struct boot_record {
    struct boot_identity latest, confirmed;
    int is_confirmed, remaining;
};

static int boot_identity_read(const char *text, const char *prefix, struct boot_identity *identity)
{
    const char *names[] = {"slot", "generation", "release", "initramfs"};
    char *values[] = {identity->slot, identity->generation, identity->release, identity->initramfs};
    const size_t capacities[] = {sizeof(identity->slot), sizeof(identity->generation),
        sizeof(identity->release), sizeof(identity->initramfs)};
    int result = 0;
    for (size_t i = 0; !result && i < 4; ++i) {
        char key[48]; snprintf(key, sizeof(key), "%s_%s", prefix, names[i]);
        result = record_field(text, key, values[i], capacities[i]);
    }
    if (result) return result;
    errno = 0; char *end; unsigned long generation = strtoul(identity->generation, &end, 10);
    if ((strcmp(identity->slot, "A") && strcmp(identity->slot, "B")) ||
        identity->generation[0] < '1' || identity->generation[0] > '9' || *end || errno ||
        !generation || generation > 2147483647ul || !scalar(identity->release, 31) ||
        !update_token(identity->initramfs)) return -EINVAL;
    return 0;
}

static int boot_identity_equal(const struct boot_identity *a, const struct boot_identity *b)
{
    return !strcmp(a->slot, b->slot) && !strcmp(a->generation, b->generation) &&
        !strcmp(a->release, b->release) && !strcmp(a->initramfs, b->initramfs);
}

static int boot_record_read(const char *text, struct boot_record *record)
{
    char version[2], protocol[2], confirmed[2], remaining[2];
    int result = record_field(text, "format_version", version, sizeof(version));
    if (!result) result = record_field(text, "boot_protocol", protocol, sizeof(protocol));
    if (!result) result = record_field(text, "latest_confirmed", confirmed, sizeof(confirmed));
    if (!result) result = record_field(text, "latest_remaining", remaining, sizeof(remaining));
    if (!result && (strcmp(version, "1") || strcmp(protocol, "1") ||
        (strcmp(confirmed, "0") && strcmp(confirmed, "1")) ||
        (strcmp(remaining, "0") && strcmp(remaining, "1")))) result = -EINVAL;
    if (!result) result = boot_identity_read(text, "latest", &record->latest);
    if (!result) result = boot_identity_read(text, "confirmed", &record->confirmed);
    if (result) return result;
    record->is_confirmed = confirmed[0] - '0'; record->remaining = remaining[0] - '0';
    if (record->is_confirmed) {
        if (record->remaining || !boot_identity_equal(&record->latest, &record->confirmed)) return -EINVAL;
    } else if (!strcmp(record->latest.slot, record->confirmed.slot) ||
        strtoul(record->latest.generation, NULL, 10) <= strtoul(record->confirmed.generation, NULL, 10)) return -EINVAL;
    return 0;
}

static int candidate_matches(const struct boot_identity *identity, const char *expected, int *matches)
{
    char release[32], initramfs[65];
    int result = record_field(expected, "kernel_release", release, sizeof(release));
    if (!result) result = record_field(expected, "initramfs_sha256", initramfs, sizeof(initramfs));
    if (!result && (!scalar(release, 31) || !update_token(initramfs))) result = -EINVAL;
    if (!result) *matches = !strcmp(identity->release, release) && !strcmp(identity->initramfs, initramfs);
    return result;
}

/* Consuming a GRUB attempt is not evidence that the fallback is now running.
 * A real-root replacement after failure must be invoked from that exact last
 * confirmed boot. Isolated roots have no running guest to authenticate. */
static int running_confirmed(struct update_context *context, const struct boot_identity *identity)
{
    if (context->isolated) return 0;
    struct utsname system;
    if (uname(&system)) return -errno;
    if (strcmp(system.release, identity->release)) return -EBUSY;
    char path[PATH_MAX], bytes[4097]; size_t size = 0;
    int result = update_path(path, context->root, "proc/cmdline");
    if (result) return result;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -errno;
    /* procfs reports a zero st_size while still returning a full stream. */
    while (size < sizeof(bytes)-1) {
        ssize_t count = read(fd, bytes + size, sizeof(bytes)-1-size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -errno; break; }
        if (!count) break;
        size += (size_t)count;
    }
    close(fd);
    if (result) return result;
    if (size == sizeof(bytes)-1 || memchr(bytes, 0, size)) return -E2BIG;
    bytes[size] = 0;
    const char *names[] = {"reliefos.boot_slot=", "reliefos.boot_generation=", "reliefos.boot_initramfs="};
    const char *values[] = {identity->slot, identity->generation, identity->initramfs};
    unsigned seen = 0; char *save;
    for (char *token = strtok_r(bytes, " \t\r\n", &save); token; token = strtok_r(NULL, " \t\r\n", &save)) {
        for (size_t i = 0; i < 3; ++i) {
            size_t length = strlen(names[i]);
            if (strncmp(token, names[i], length)) continue;
            if ((seen & (1u << i)) || strcmp(token + length, values[i])) result = -EBUSY;
            seen |= 1u << i;
        }
    }
    return result ? result : seen == 7 ? 0 : -EBUSY;
}

int update_kernel_preflight(struct update_context *context)
{
    char selector[PATH_MAX], *bytes = NULL; size_t size;
    int result = update_path(selector, context->root, "boot/grub/kernel-slots.cfg");
    if (!result) result = update_read(selector, &bytes, &size, 1024u*1024u);
    if (!result && !strstr(bytes, "# reliefos-boot-health-v1")) result = -EPROTONOSUPPORT;
    free(bytes);
    if (result) {
        fputs("[reliefos-update] boot resources require migration with matching installation media before kernel updates\n", stderr);
        return result;
    }
    char boot[PATH_MAX], path[PATH_MAX], *expected = NULL; bytes = NULL;
    result = update_path(boot, context->root, "boot");
    if (!result) result = update_path(path, context->plan, "boot-before");
    char *arguments[] = {context->bundle, "--boot-status", context->root, boot, NULL};
    if (!result) result = update_run(context, arguments, path, 0);
    if (!result) result = update_read(path, &bytes, &size, 2048);
    struct boot_record record;
    if (!result) result = boot_record_read(bytes, &record);
    if (!result && !record.is_confirmed) {
        if (record.remaining) {
            fputs("[reliefos-update] previous kernel trial is unconfirmed; boot and confirm it before another kernel update\n", stderr);
            result = -EBUSY;
        } else {
            result = update_path(path, context->plan, "kernel-identity");
            if (!result) result = update_read(path, &expected, &size, 1024);
            int same = 0;
            if (!result) result = candidate_matches(&record.latest, expected, &same);
            if (!result && same) {
                fputs("[reliefos-update] this kernel trial already failed; refusing to reset its boot attempt. Prepare a different kernel/initramfs release from the confirmed recovery boot\n", stderr);
                result = -EBUSY;
            }
            if (!result) result = running_confirmed(context, &record.confirmed);
            if (result == -EBUSY && !same)
                fputs("[reliefos-update] boot the confirmed recovery kernel before replacing the failed trial\n", stderr);
        }
    }
    free(bytes); free(expected); return result;
}

int update_kernel_publish(struct update_context *context)
{
    char path[PATH_MAX], boot[PATH_MAX], *before = NULL, *after = NULL, *expected = NULL;
    size_t size; int result = update_path(boot, context->root, "boot");
    char *publish[] = {context->bundle, "--publish-installed", context->root, boot, NULL};
    if (!result) result = update_run(context, publish, NULL, 1);
    if (!result) result = update_path(path, context->plan, "boot-after");
    char *status[] = {context->bundle, "--boot-status", context->root, boot, NULL};
    if (!result) result = update_run(context, status, path, 1);
    if (!result) result = update_read(path, &after, &size, 2048);
    if (!result) result = update_path(path, context->plan, "boot-before");
    if (!result) result = update_read(path, &before, &size, 2048);
    if (!result) result = update_path(path, context->plan, "installed-kernel-identity");
    if (!result) result = update_read(path, &expected, &size, 1024);
    struct boot_record previous, current; int matched = 0, already_published = 0;
    if (!result) result = boot_record_read(before, &previous);
    if (!result) result = boot_record_read(after, &current);
    if (!result) result = candidate_matches(&current.latest, expected, &matched);
    if (!result && !matched) result = -EBADMSG;
    if (!result) result = candidate_matches(&previous.latest, expected, &already_published);
    if (!result && already_published && !boot_identity_equal(&previous.latest, &current.latest)) result = -EBADMSG;
    if (!result && !already_published &&
        (strtoul(current.latest.generation, NULL, 10) <= strtoul(previous.latest.generation, NULL, 10) ||
         !strcmp(current.latest.slot, previous.confirmed.slot))) result = -EBADMSG;
    /* An unchanged confirmed payload needs no new trial. A published trial may
     * also have booted and been confirmed before an interrupted commit repairs.
     * For every still-unconfirmed candidate retain the exact previous fallback. */
    if (!result && !current.is_confirmed && !boot_identity_equal(&previous.confirmed, &current.confirmed)) result = -EBADMSG;
    if (!result && !current.is_confirmed && !current.remaining)
        fputs("[reliefos-update] the published kernel trial was consumed without confirmation; the next automatic boot uses the confirmed recovery kernel\n", stderr);
    if (!result && current.is_confirmed) context->state.reboot = 0;
    free(before); free(after); free(expected); return result;
}
