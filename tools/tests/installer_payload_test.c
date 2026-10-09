#define _GNU_SOURCE
#include <linux/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW
#define lstat fixture_lstat
#include "../../userland/apps/installer/install_ops.c"
#undef lstat

/* Map guest stat requests into a real, private host filesystem fixture. */
static const char *fixture_root;
int fixture_lstat(const char *path, struct stat *out)
{
    char translated[4096];
    int size = snprintf(translated, sizeof(translated), "%s%s", fixture_root, path);
    if (size < 0 || (size_t)size >= sizeof(translated)) { errno = ENAMETOOLONG; return -1; }
    return fstatat(AT_FDCWD, translated, out, AT_SYMLINK_NOFOLLOW);
}
int reliefos_stat_legacy(const char *path, struct reliefos_stat *out)
{
    char translated[4096];
    struct stat value;
    int size = snprintf(translated, sizeof(translated), "%s%s", fixture_root, path);
    if (size < 0 || (size_t)size >= sizeof(translated)) return -ENAMETOOLONG;
    if (stat(translated, &value) < 0) return -errno;
    memset(out, 0, sizeof(*out));
    out->type = S_ISDIR(value.st_mode) ? RELIEFOS_FS_TYPE_DIR :
                S_ISREG(value.st_mode) ? RELIEFOS_FS_TYPE_FILE :
                RELIEFOS_FS_TYPE_SYMLINK;
    out->size = value.st_size;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    fixture_root = argv[1];
    int result = argc == 3 ? check_update_target_required() : check_update_payload_required();
    printf("payload-check=%d\n", result);
    return result == 0 ? 0 : 1;
}
