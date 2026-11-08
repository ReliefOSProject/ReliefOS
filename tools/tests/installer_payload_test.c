#define _GNU_SOURCE
#include <linux/mount.h>
#undef MNT_FORCE
#undef MNT_DETACH
#undef MNT_EXPIRE
#undef UMOUNT_NOFOLLOW
#define lstat fixture_lstat
#define fopen fixture_fopen
#include "../../userland/apps/installer/install_ops.c"
#undef lstat
#undef fopen
#include "../../userland/apps/installer/installer_update.h"

/* Map guest stat requests into a real, private host filesystem fixture. */
static const char *fixture_root;
FILE *fixture_fopen(const char *path, const char *mode)
{
    char translated[4096];
    int size = snprintf(translated, sizeof(translated), "%s%s", fixture_root, path);
    if (size < 0 || (size_t)size >= sizeof(translated)) { errno = ENAMETOOLONG; return NULL; }
    int fd = open(translated, O_RDONLY);
    if (fd < 0) return NULL;
    FILE *stream = fdopen(fd, mode);
    if (!stream) { int error = errno; close(fd); errno = error; }
    return stream;
}
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
    int result;
    if (argc == 3 && !strcmp(argv[2], "fresh-xorg")) {
        setup.skip_xorg = 0;
        result = check_install_payload_required();
    } else if (argc == 3 && !strcmp(argv[2], "fresh-console")) {
        setup.skip_xorg = 1;
        result = check_install_payload_required();
    } else if (argc == 3 && !strcmp(argv[2], "update")) {
        struct installer_update_info target = {0}, media = {0};
        char root[4096], payload[4096];
        snprintf(root, sizeof(root), "%s/target", fixture_root);
        result = installer_update_read_system(root, &target);
        if (!result) {
            snprintf(payload, sizeof(payload), "%s/install/%s", fixture_root,
                     !strcmp(target.profile, "console") ? "root-console" : "root");
            result = installer_update_read_media(payload, target.profile, &media);
        }
    } else if (argc == 3) {
        struct installer_update_info target = {0};
        char root[4096];
        snprintf(root, sizeof(root), "%s/target", fixture_root);
        result = installer_update_read_system(root, &target);
    } else result = check_install_payload_required();
    printf("payload-check=%d\n", result);
    return result == 0 ? 0 : 1;
}
