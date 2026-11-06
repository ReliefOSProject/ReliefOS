#include <assert.h>
#include "../../userland/apps/installer/installer_setup.c"

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *fixture_root=argv[1];
    struct reliefos_user_info user = {.uid = getuid(), .home = "/home/test"};
    if (prepare_home(fixture_root, &user) < 0) {
        int error = errno;
        fprintf(stderr, "Xorg home setup failed: %s (errno=%d)\n", strerror(error), error);
        return error;
    }
    return 0;
}
