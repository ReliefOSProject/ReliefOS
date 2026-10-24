#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../userland/apps/installer/installer_setup.h"

int main(void)
{
    struct installer_setup setup = {0};
    assert(setup.skip_xorg == 0);
    assert(setup.skip_bios == 0 && setup.skip_uefi == 0);
    strcpy(setup.username, "alice");
    strcpy(setup.password, "user!");
    strcpy(setup.password_confirm, "user!");
    strcpy(setup.root_password, "root!");
    strcpy(setup.root_password_confirm, "root!");
    assert(installer_setup_valid(&setup));
    for (unsigned modes = 0; modes < 4; ++modes) {
        setup.skip_bios = modes & 1;
        setup.skip_uefi = (modes >> 1) & 1;
        assert(installer_setup_valid(&setup));
    }
    setup.skip_bios = 2;
    assert(!installer_setup_valid(&setup));
    setup.skip_bios = setup.skip_uefi = 0;
    strcpy(setup.username, "root");
    assert(!installer_setup_valid(&setup));
    strcpy(setup.username, "../alice");
    assert(!installer_setup_valid(&setup));
    strcpy(setup.username, "alice");
    setup.root_password[0] = 0;
    setup.root_password_confirm[0] = 0;
    assert(!installer_setup_valid(&setup));
    strcpy(setup.root_password, "a b");
    strcpy(setup.root_password_confirm, "a b");
    assert(!installer_setup_valid(&setup));
    puts("installer account validation PASS");
}
