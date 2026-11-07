#ifndef LEONOS_AUTH_H
#define LEONOS_AUTH_H

#include <reliefos/auth_user.h>

/* Historical regression fixture only: the retired authd protocol keeps its
 * pre-rename type and constant spellings, aliased here to the canonical UAPI
 * names so the fixture and its tests share one published layout. */
#define LEONOS_AUTH_HOME_LEN RELIEFOS_AUTH_HOME_LEN
#define LEONOS_AUTH_PASSWORD_LEN RELIEFOS_AUTH_PASSWORD_LEN
#define LEONOS_AUTH_PASSWORD_MAX_CHARS RELIEFOS_AUTH_PASSWORD_MAX_CHARS
#define LEONOS_AUTH_PASSWORD_MIN_CHARS RELIEFOS_AUTH_PASSWORD_MIN_CHARS
#define LEONOS_AUTH_USERNAME_LEN RELIEFOS_AUTH_USERNAME_LEN
#define leonos_user_info reliefos_user_info
#define leonos_auth_record reliefos_auth_record

#define LEONOS_AUTH_ROLE_NONE 0U
#define LEONOS_AUTH_ROLE_USER 1U
#define LEONOS_AUTH_ROLE_ADMIN 2U

#define LEONOS_AUTH_USER_DISABLED 0x00000001U


#define LEONOS_AUTH_OP_STATUS 1U
#define LEONOS_AUTH_OP_LIST_USERS 2U
#define LEONOS_AUTH_OP_LOGIN 3U
#define LEONOS_AUTH_OP_CREATE_USER 4U
#define LEONOS_AUTH_OP_UPDATE_USER 5U
#define LEONOS_AUTH_OP_CHANGE_PASSWORD 6U
#define LEONOS_AUTH_OP_AUTHORIZE 7U
#define LEONOS_AUTH_OP_FSPERM 8U

#define LEONOS_AUTH_UPDATE_ROLE 0x00000001U
#define LEONOS_AUTH_UPDATE_FLAGS 0x00000002U

#define LEONOS_AUTHZ_READ 1U
#define LEONOS_AUTHZ_WRITE 2U
#define LEONOS_AUTHZ_EXEC 3U
#define LEONOS_AUTHZ_USER_ADMIN 4U
#define LEONOS_AUTHZ_INSTALL 5U
#define LEONOS_AUTHZ_KILL_TASK 6U
#define LEONOS_AUTHZ_DELETE 7U
#define LEONOS_AUTHZ_MANAGE 8U

#define LEONOS_AUTHZ_ACTOR_SERVICE 0x00000001U

struct leonos_auth_status {
    uint32_t user_count;
    uint32_t has_admin;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct leonos_user_list {
    uint32_t actor_uid;
    uint32_t actor_role;
    uint32_t include_disabled;
    uint32_t capacity;
    uint32_t count;
    uint32_t reserved;
    struct leonos_user_info *users;
};

struct leonos_auth_login {
    char username[LEONOS_AUTH_USERNAME_LEN];
    char password[LEONOS_AUTH_PASSWORD_LEN];
    struct leonos_user_info user;
};

struct leonos_auth_delegate_elevation {
    uint32_t child_pid;
    uint32_t reserved;
};

struct leonos_auth_create {
    uint32_t actor_uid;
    uint32_t actor_role;
    uint32_t role;
    uint32_t reserved;
    char username[LEONOS_AUTH_USERNAME_LEN];
    char password[LEONOS_AUTH_PASSWORD_LEN];
    struct leonos_user_info user;
};

struct leonos_auth_update {
    uint32_t actor_uid;
    uint32_t actor_role;
    uint32_t uid;
    uint32_t mask;
    uint32_t role;
    uint32_t flags;
};

struct leonos_auth_password {
    uint32_t actor_uid;
    uint32_t actor_role;
    uint32_t uid;
    uint32_t reserved;
    char old_password[LEONOS_AUTH_PASSWORD_LEN];
    char new_password[LEONOS_AUTH_PASSWORD_LEN];
};

struct leonos_authz_request {
    uint32_t uid;
    uint32_t role;
    uint32_t session_id;
    uint32_t op;
    uint32_t target_uid;
    uint32_t target_role;
    uint32_t allowed;
    uint32_t actor_flags;
    char username[LEONOS_AUTH_USERNAME_LEN];
    char home[LEONOS_AUTH_HOME_LEN];
    char path[256];
};

/* Between 1 and 32 UTF-8 characters, with no whitespace. */
int leonos_auth_password_valid(const char *password, uint32_t capacity);
int leonos_auth_status(struct leonos_auth_status *status);
int leonos_auth_current(struct leonos_user_info *user);
int leonos_auth_list_users(struct leonos_user_info *users, uint32_t capacity,
                           uint32_t include_disabled, uint32_t *out_count);
int leonos_auth_users_alloc(struct leonos_user_info **users, uint32_t include_disabled,
                            uint32_t *out_count);
int leonos_auth_logout(void);
int leonos_auth_create_user(const char *username, const char *password,
                            uint32_t role, struct leonos_user_info *user);
int leonos_auth_update_user(uint32_t uid, uint32_t mask, uint32_t role,
                            uint32_t flags);
int leonos_auth_request_power(uint32_t command);

#endif
