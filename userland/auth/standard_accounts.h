#ifndef RELIEFOS_STANDARD_ACCOUNTS_H
#define RELIEFOS_STANDARD_ACCOUNTS_H
#include <reliefos/auth.h>
#include <pwd.h>

int reliefos_account_info(const struct passwd *account, struct reliefos_user_info *out);
int reliefos_account_name_valid(const char *name, unsigned capacity);
/* Check that the read-only installation payload has room for the new login. */
int reliefos_account_seed_check(const char *target, const char *name);
int reliefos_account_seed(const char *target, const char *name,
                        const char *password, const char *root_password);
int reliefos_account_legacy_check(const char *target);
#endif
