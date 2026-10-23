#include <assert.h>
#include "../../userland/apps/installer/installer_setup.c"
#include <reliefos/fs.h>

static const char *fixture_root;

/* Resolve runtime application lookups inside the private media fixture while
 * keeping the real launcher's validation and shortcut writes. */
int reliefos_stat_legacy(const char *path, struct reliefos_stat *out)
{
    char translated[1024];
    struct stat status;
    if (!strncmp(path, "/usr/", 5)) {
        int length = snprintf(translated, sizeof(translated), "%s%s", fixture_root, path);
        assert(length > 0 && (size_t)length < sizeof(translated));
        path = translated;
    }
    if (stat(path, &status) < 0) return -errno;
    memset(out, 0, sizeof(*out));
    out->type = S_ISDIR(status.st_mode) ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
    out->size = status.st_size;
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    fixture_root = argv[1];
    struct reliefos_user_info user = {.uid = getuid(), .home = "/home/test"};
    if (prepare_home(fixture_root, &user) < 0) {
        int error = errno;
        fprintf(stderr, "Xorg home setup failed: %s (errno=%d)\n", strerror(error), error);
        return error;
    }
    return 0;
}
