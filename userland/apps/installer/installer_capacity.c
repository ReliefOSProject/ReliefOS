#include "installer_capacity.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** @brief Recommend one MiB-rounded RAM size within the documented defaults.
 * @param ram_kib Detected RAM in KiB. @return Recommended whole MiB, 512..2048.
 */
uint64_t installer_swap_recommend_mib(uint64_t ram_kib)
{
    uint64_t mib=ram_kib/1024+(ram_kib%1024!=0);
    return mib<512 ? 512 : mib>2048 ? 2048 : mib;
}

/** @brief Compute the shared default from the running machine's reported RAM.
 * @return Recommended whole MiB, falling back to 512 when reporting is unavailable.
 */
uint64_t installer_swap_recommend_system(void)
{
    FILE *file=fopen("/proc/meminfo","r");
    if (!file) return 512;
    char line[128];uint64_t recommendation=512;
    while (fgets(line,sizeof(line),file)) {
        if (strncmp(line,"MemTotal:",9)) continue;
        const char *start=line+9;
        while (*start==' ' || *start=='\t') ++start;
        if (*start<'0' || *start>'9') break;
        errno=0;char *end;
        unsigned long long kib=strtoull(start,&end,10);
        while (*end==' ' || *end=='\t') ++end;
        if (!errno && (!strcmp(end,"kB\n") || !strcmp(end,"kB")))
            recommendation=installer_swap_recommend_mib((uint64_t)kib);
        break;
    }
    fclose(file);return recommendation;
}

/** @brief Accept positive whole MiB representable by the v1 last-page field.
 * @param mib Proposed whole MiB. @return Nonzero exactly when representable.
 */
int installer_swap_size_valid(uint64_t mib)
{
    return mib && mib<=((uint64_t)UINT32_MAX+1u)/256u;
}

/** @brief Parse a decimal MiB field and publish only a complete valid value.
 * @param text NUL-terminated decimal text. @param mib Writable output changed
 * only on success. @return Zero, negative EINVAL or negative EOVERFLOW.
 */
int installer_swap_parse_mib(const char *text,uint64_t *mib)
{
    if (!text || !*text || !mib) return -EINVAL;
    uint64_t value=0;
    for (const char *p=text;*p;++p) {
        if (*p<'0' || *p>'9') return -EINVAL;
        unsigned digit=(unsigned)(*p-'0');
        if (value>(UINT64_MAX-digit)/10u) return -EOVERFLOW;
        value=value*10u+digit;
    }
    if (!installer_swap_size_valid(value)) return -EINVAL;
    *mib=value;return 0;
}

/** @brief Add the selected allocated file without partially updating need.
 * @param need Mutable capacity snapshot. @param mib Selected whole MiB.
 * @return Zero or negative validation/arithmetic errno, leaving need unchanged.
 */
int installer_capacity_add_swap(struct installer_capacity *need,uint64_t mib)
{
    if (!need || !installer_swap_size_valid(mib)) return -EINVAL;
    uint64_t bytes=mib*1024u*1024u;
    if (need->root_bytes>UINT64_MAX-bytes || need->root_inodes==UINT64_MAX)
        return -EOVERFLOW;
    need->root_bytes+=bytes; ++need->root_inodes;
    return 0;
}

/** @brief Accumulate with checked arithmetic and a filesystem allocation unit. */
static int account(uint64_t *total, uint64_t size, uint64_t unit)
{
    if (size > UINT64_MAX - (unit - 1)) return -EOVERFLOW;
    size = (size + unit - 1) / unit * unit;
    if (*total > UINT64_MAX - size) return -EOVERFLOW;
    *total += size;
    return 0;
}

/** @brief Walk bounded paths; count links themselves instead of their referents. */
static int measure(const char *base, const char *relative, unsigned depth,
                    int boot, int bios, int uefi, struct installer_capacity *need)
{
    if (depth > 64) return -ELOOP;
    if (boot && ((!uefi && !strcmp(relative, "EFI")) ||
                 (!bios && !strcmp(relative, "grub/i386-pc")))) return 0;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s%s%s", base, *relative ? "/" : "", relative);
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;
    struct stat st;
    if (lstat(path, &st) < 0) return -errno;
    if (!*relative && !S_ISDIR(st.st_mode)) return -ELOOP;
    if (st.st_size < 0) return -EINVAL;
    if (boot && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return -ELOOP;
    uint64_t size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : (boot ? 32768 : 4096);
    int ret = account(boot ? &need->boot_bytes : &need->root_bytes, size, boot ? 32768 : 4096);
    if (ret) return ret;
    if (!boot) {
        if (need->root_inodes == UINT64_MAX) return -EOVERFLOW;
        ++need->root_inodes;
    }
    if (!S_ISDIR(st.st_mode)) return 0;
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) ret = -errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[1024];
        n = snprintf(child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { ret = -ENAMETOOLONG; break; }
        ret = measure(base, child, depth + 1, boot, bios, uefi, need);
        if (ret) break;
    }
    if (closedir(dir) < 0 && !ret) ret = -errno;
    return ret;
}

/** @brief Publish a complete measurement only after both traversals succeed. */
int installer_capacity_measure(const char *root, const char *boot, int bios, int uefi,
                               struct installer_capacity *out)
{
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!root || !boot || (bios != 0 && bios != 1) || (uefi != 0 && uefi != 1)) return -EINVAL;
    struct installer_capacity need = {0};
    int ret = measure(root, "", 0, 0, bios, uefi, &need);
    if (!ret) ret = measure(boot, "", 0, 1, bios, uefi, &need);
    if (!ret) *out = need;
    return ret;
}

/** @brief Reserve ext4 journal/metadata, FAT metadata, and post-copy configuration space. */
int installer_capacity_check(const struct installer_capacity *need,
                             uint64_t root_sectors, uint64_t boot_sectors)
{
    if (!need) return -EINVAL;
    if (root_sectors > UINT64_MAX / 512 || boot_sectors > UINT64_MAX / 512) return -EOVERFLOW;
    uint64_t root = root_sectors * 512, boot = boot_sectors * 512;
    /* Explicit mkfs settings below use 4KiB blocks and one inode per 16KiB.
     * Reserve 12.5% plus 64MiB for ext4 metadata/journal/reserved blocks and
     * generated files. FAT uses 32KiB rounding, 12.5% plus 8MiB spare space. */
    uint64_t root_spare = root / 8 + 64ULL * 1024 * 1024;
    uint64_t boot_spare = boot / 8 + 8ULL * 1024 * 1024;
    if (root < root_spare || boot < boot_spare ||
        need->root_bytes > root - root_spare || need->boot_bytes > boot - boot_spare ||
        root / 16384 < 32 || need->root_inodes > root / 16384 - 32) return -ENOSPC;
    return 0;
}
