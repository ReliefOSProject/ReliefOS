#include <assert.h>
#include "../../userland/apps/installer/installer_setup.c"

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct reliefos_user_info user = {.uid = getuid(), .home = "/home/test"};
    assert(prepare_home(argv[1], &user) == 0);
    return 0;
}
