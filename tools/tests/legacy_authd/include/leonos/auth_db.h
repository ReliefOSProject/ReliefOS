#ifndef LEONOS_AUTH_MAX_USERS
#define LEONOS_AUTH_MAX_USERS 32U
#endif
#ifndef LEONOS_UAPI_AUTH_DB_H
#define LEONOS_UAPI_AUTH_DB_H
#include <reliefos/auth_user.h>
#include <leonos/layout.h>

/* Historical regression fixture only: the pre-rename type spellings alias
 * the canonical UAPI structs so the fixture and its tests share one type. */
#define leonos_user_info reliefos_user_info
#define leonos_auth_record reliefos_auth_record

#define LEONOS_AUTH_DB_PATH LEONOS_PATH_USERS_DB
#define LEONOS_AUTH_DB_MAGIC 0x41555332U
#define LEONOS_AUTH_HASH_LEN 128U

/* AUS2: NUL-terminated PBKDF2-HMAC-SHA256 strings with a random 128-bit salt. */
struct leonos_auth_record {
    struct leonos_user_info user;
    uint8_t password_hash[LEONOS_AUTH_HASH_LEN];
};
struct leonos_auth_database {
    uint32_t magic;
    uint32_t count;
    struct leonos_auth_record users[LEONOS_AUTH_MAX_USERS];
};
#endif
