#define main worker_main
#include "../../userland/apps/sudod/main.c"
#undef main
#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"

int main(void)
{
    char root[192];
    host_tmp_path(root, sizeof(root), "sudod-XXXXXX");
    assert(mkdtemp(root));
    char link[512], child[512], name[RELIEFOS_FS_PATH_LEN];
    snprintf(link, sizeof(link), "%s/link", root);
    snprintf(child, sizeof(child), "%s/link/self", root);
    assert(symlink("/proc", link) == 0);
    assert(sudod_open_dir(child) == -1);
    assert(sudod_parent_fd(child, name) == -1);
    assert(sudod_open_dir("//proc/self") == -1);
    int fd = sudod_open_dir(root);
    assert(fd >= 0);
    assert(mkdirat(fd, "real", 0700) == 0);
    assert(unlinkat(fd, "real", AT_REMOVEDIR) == 0);
    close(fd);
    assert(unlink(link) == 0 && rmdir(root) == 0);
    puts("sudod paths: canonical paths and symlink-parent rejection PASS");
    return 0;
}
