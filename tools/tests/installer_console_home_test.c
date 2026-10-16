#include <assert.h>
#include "../../userland/apps/installer/installer_setup.c"

int reliefos_launch_create_shortcut_in_dir(const char *directory, const char *executable,
                                          char *path, uint32_t capacity)
{
    (void)directory; (void)executable; (void)path; (void)capacity;
    assert(!"console installation must not create desktop shortcuts");
    return -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct reliefos_user_info user = {.uid = getuid(), .home = "/home/test"};
    assert(prepare_home(argv[1], &user, 1) == 0);
    return 0;
}
