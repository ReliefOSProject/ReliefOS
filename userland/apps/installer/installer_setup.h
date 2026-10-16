#ifndef RELIEFOS_INSTALLER_SETUP_H
#define RELIEFOS_INSTALLER_SETUP_H
#include <reliefos/auth_user.h>
#include <stdint.h>

struct installer_setup {
    char username[RELIEFOS_AUTH_USERNAME_LEN];
    char password[RELIEFOS_AUTH_PASSWORD_LEN];
    char password_confirm[RELIEFOS_AUTH_PASSWORD_LEN];
    char root_password[RELIEFOS_AUTH_PASSWORD_LEN];
    char root_password_confirm[RELIEFOS_AUTH_PASSWORD_LEN];
    uint8_t skip_xorg;
};

int installer_setup_valid(const struct installer_setup *setup);
int installer_setup_write(const struct installer_setup *setup, const char *target);
#endif
