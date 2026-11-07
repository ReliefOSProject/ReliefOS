/* Historical regression fixture only. Never build or stage into LeonOS. */
/* authd: LeonOS authentication daemon. Runs as uid==0 and owns
 * /var/lib/leonos/users.db. Every frame's SCM_CREDENTIALS must match the
 * accept-time SO_PEERCRED identity. Stable PID lifetime binding is pending. */
#include <errno.h>
#include <leonos/auth.h>
#include <leonos/auth_db.h>
#include <leonos/authd.h>
#include <leonos/fs.h>
#include <leonos/stdio.h>
#include <leonos/syscall.h>
#include <leonos/unix_ipc.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include "accounts.h"
#include "authd_sudo.h"
#include <leonos/layout.h>

#define AUTHD_USERS_DB LEONOS_AUTH_DB_PATH
#define AUTHD_SESSION_FILE "/run/leonos/session-user"
#define AUTHD_MAGIC LEONOS_AUTH_DB_MAGIC
#define AUTHD_HASH_LEN LEONOS_AUTH_HASH_LEN
#define AUTHD_MAX_CLIENTS 16u
#define AUTHD_FRAME_CAP 4096u

static struct leonos_auth_record users[LEONOS_AUTH_MAX_USERS];
static uint32_t user_count;
static uint32_t current_uid;
static int session_active;
static int listen_fd = -1;

struct authd_client {
    uint32_t used;
    int fd;
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
    int stdio[3];
    uint32_t stdio_mask;
};

static struct authd_client clients[AUTHD_MAX_CLIENTS];

static void authd_copy(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || !capacity) return;
    while (src && src[i] && i + 1u < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int authd_text_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

static int authd_text_valid(const char *text, uint32_t capacity)
{
    return leonos_auth_password_valid(text, capacity);
}


static int authd_save(void);

static int authd_load(void)
{
    uint32_t magic = 0;
    uint32_t count = 0;
    int fd = open(AUTHD_USERS_DB, LEONOS_O_RDONLY, 0);
    uint32_t got = 0;
    if (fd < 0) return fd;
    long first = read(fd, &magic, sizeof(magic));
    if (first != sizeof(magic) ||
        read(fd, &count, sizeof(count)) != sizeof(count) ||
        magic != AUTHD_MAGIC || count > LEONOS_AUTH_MAX_USERS) {
        close(fd);
        errno = EIO;
        return -1;
    }
    while (got < count) {
        long n = read(fd, &users[got], sizeof(users[got]));
        if (n != (long)sizeof(users[got])) { close(fd); errno = EIO; return -1; }
        struct leonos_user_info *user = &users[got].user;
        if (!authd_account_valid(user) ||
            !memchr(users[got].password_hash, 0, sizeof(users[got].password_hash))) {
            close(fd); errno = EIO; return -1;
        }
        ++got;
    }
    close(fd);
    user_count = count;
    return 0;
}

static int authd_save(void)
{
    return authd_store_database(AUTHD_USERS_DB, users, user_count);
}

static struct leonos_auth_record *authd_find_uid(uint32_t uid)
{
    for (uint32_t i = 0; i < user_count; ++i) {
        if (users[i].user.uid == uid) return &users[i];
    }
    return 0;
}

static struct leonos_auth_record *authd_find_name(const char *name)
{
    for (uint32_t i = 0; i < user_count; ++i) {
        if (authd_text_eq(users[i].user.username, name)) return &users[i];
    }
    return 0;
}

static void authd_fill_status(struct leonos_auth_status *status)
{
    status->user_count = user_count;
    status->has_admin = 0;
    for (uint32_t i = 0; i < user_count; ++i) {
        if ((users[i].user.flags & LEONOS_AUTH_USER_DISABLED) == 0 &&
            users[i].user.role == LEONOS_AUTH_ROLE_ADMIN) {
            status->has_admin = 1;
        }
    }
}

static int authd_verify(const struct leonos_auth_record *record, const char *password)
{
    return authd_check_password(record, password);
}

static int authd_ensure_dir(const char *path)
{
    struct leonos_stat st;
    if (!path || !path[0]) {
        return -1;
    }
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    st = (struct leonos_stat){0};
    return leonos_stat_legacy(path, &st) == 0 && st.type == LEONOS_FS_TYPE_DIR ? 0 : -1;
}

static int authd_prepare_user_home(const struct leonos_user_info *user)
{
    if (!user || !user->home[0]) return -1;
    if (authd_ensure_dir("/home")<0 || authd_ensure_dir(user->home)<0) return -1;
    return chown(user->home,user->uid,user->uid)<0 || chmod(user->home,0700)<0 ? -1 : 0;
}

static void authd_send_ack(int slot, int32_t code)
{
    struct leonos_authd_ack ack = {.code = code};
    if (slot >= 0 && slot < AUTHD_MAX_CLIENTS && clients[slot].used) {
        (void)leonos_ipc_send(clients[slot].fd, LEONOS_AUTHD_MSG_ACK, &ack,
                              sizeof(ack));
    }
}

static void authd_handle_login(int slot, const uint8_t *buffer, uint32_t length)
{
    struct leonos_auth_login login;
    struct leonos_auth_record *record;
    if (length != sizeof(login) || clients[slot].uid != 0) { authd_send_ack(slot, -EACCES); return; }
    memcpy(&login, buffer, sizeof(login));
    if (!authd_username_valid(login.username, sizeof(login.username)) ||
        !authd_text_valid(login.password, sizeof(login.password))) {
        memset(login.password, 0, sizeof(login.password));
        authd_send_ack(slot, -1);
        return;
    }
    record = authd_find_name(login.username);
    if (!record || (record->user.flags & LEONOS_AUTH_USER_DISABLED) ||
        !authd_verify(record, login.password)) {
        memset(login.password, 0, sizeof(login.password));
        authd_send_ack(slot, -1);
        return;
    }
    memset(login.password, 0, sizeof(login.password));
    if (authd_publish_session(AUTHD_SESSION_FILE, &record->user) < 0) {
        authd_send_ack(slot, -errno);
        return;
    }
    current_uid = record->user.uid;
    session_active = 1;
    (void)leonos_ipc_send(clients[slot].fd, LEONOS_AUTHD_MSG_LOGIN,
                          &record->user, sizeof(record->user));
}

static void authd_handle_elevate(int slot, const uint8_t *buffer, uint32_t length)
{
    struct leonos_auth_login login;
    struct leonos_auth_record *record;
    if (length < sizeof(login) || clients[slot].uid != 0) { authd_send_ack(slot, -1); return; }
    memcpy(&login, buffer, sizeof(login));
    if (!authd_username_valid(login.username, sizeof(login.username)) ||
        !authd_text_valid(login.password, sizeof(login.password))) {
        memset(login.password, 0, sizeof(login.password));
        authd_send_ack(slot, -1);
        return;
    }
    record = authd_find_name(login.username);
    if (!record || record->user.role != LEONOS_AUTH_ROLE_ADMIN ||
        (record->user.flags & LEONOS_AUTH_USER_DISABLED) ||
        !authd_verify(record, login.password)) {
        memset(login.password, 0, sizeof(login.password));
        authd_send_ack(slot, -1);
        return;
    }
    memset(login.password, 0, sizeof(login.password));
    (void)leonos_ipc_send(clients[slot].fd, LEONOS_AUTHD_MSG_ELEVATE,
                          &record->user, sizeof(record->user));
}

static void authd_handle_create(int slot, const uint8_t *buffer, uint32_t length)
{
    struct leonos_authd_create create;
    struct leonos_auth_record *record;
    uint32_t uid = 1000;
    if (length < sizeof(create)) { authd_send_ack(slot, -1); return; }
    memcpy(&create, buffer, sizeof(create));
    if (!authd_username_valid(create.username, sizeof(create.username)) ||
        !strcmp(create.username, "root") || !strcmp(create.username, "nobody") ||
        !authd_text_valid(create.password, sizeof(create.password)) ||
        create.role != LEONOS_AUTH_ROLE_USER) {
        memset(create.password, 0, sizeof(create.password));
        authd_send_ack(slot, -1);
        return;
    }
    if (user_count == LEONOS_AUTH_MAX_USERS || authd_find_name(create.username)) {
        memset(create.password, 0, sizeof(create.password));
        authd_send_ack(slot, -1);
        return;
    }
    if (clients[slot].uid != 0) {
        memset(create.password, 0, sizeof(create.password));
        authd_send_ack(slot, -1);
        return;
    }
    record = &users[user_count];
    memset(record, 0, sizeof(*record));
    while (authd_find_uid(uid)) ++uid;
    record->user.uid = uid;
    record->user.role = create.role;
    authd_copy(record->user.username, sizeof(record->user.username), create.username);
    {
        char home[LEONOS_AUTH_HOME_LEN];
        (void)snprintf(home, sizeof(home), "/home/%s", create.username);
        authd_copy(record->user.home, sizeof(record->user.home), home);
    }
    if (authd_prepare_user_home(&record->user) < 0) {
        memset(record, 0, sizeof(*record));
        authd_send_ack(slot, -1);
        return;
    }
    if (authd_set_password(record, create.password) < 0) {
        explicit_bzero(create.password, sizeof(create.password));
        authd_send_ack(slot, -errno);
        return;
    }
    memset(create.password, 0, sizeof(create.password));
    ++user_count;
    if (authd_save() < 0) { --user_count; authd_send_ack(slot, -1); return; }
    if (authd_export_accounts("/etc", users, user_count) < 0) {
        /* The account is already committed. Keep the in-memory database in
         * sync and report the failed publication; startup will retry it. */
        printf("[authd.elf] account uid=%u saved, passwd/group export failed errno=%d\n", uid, errno);
        authd_send_ack(slot, -1);
        return;
    }
    (void)leonos_ipc_send(clients[slot].fd, LEONOS_AUTHD_MSG_CREATE,
                          &record->user, sizeof(record->user));
}

static void authd_handle_update(int slot, const uint8_t *buffer, uint32_t length)
{
    struct leonos_authd_update update;
    struct leonos_auth_record *record;
    if (length < sizeof(update)) { authd_send_ack(slot, -1); return; }
    memcpy(&update, buffer, sizeof(update));
    if (clients[slot].uid != 0 && clients[slot].uid != update.uid) {
        authd_send_ack(slot, -1);
        return;
    }
    record = authd_find_uid(update.uid);
    if (!record) { authd_send_ack(slot, -1); return; }
    if (!record->user.uid && (update.mask & LEONOS_AUTH_UPDATE_FLAGS) &&
        (update.flags & LEONOS_AUTH_USER_DISABLED)) { authd_send_ack(slot, -EPERM); return; }
    struct leonos_user_info previous = record->user;
    if (update.mask & LEONOS_AUTH_UPDATE_ROLE) {
        if (update.role != (record->user.uid ? LEONOS_AUTH_ROLE_USER : LEONOS_AUTH_ROLE_ADMIN) ||
            clients[slot].uid != 0) {
            authd_send_ack(slot, -1);
            return;
        }
        record->user.role = update.role;
    }
    if (update.mask & LEONOS_AUTH_UPDATE_FLAGS) {
        record->user.flags = update.flags & LEONOS_AUTH_USER_DISABLED;
    }
    if (authd_save() < 0) { record->user = previous; authd_send_ack(slot, -errno); return; }
    /* A role or enable change invalidates any window opened against the old
     * account state, so an elevation cannot outlive the credentials it was
     * granted for. */
    (void)authd_sudo_cache_revoke_all();
    authd_send_ack(slot, 1);
}

static void authd_handle_password(int slot, const uint8_t *buffer, uint32_t length)
{
    struct leonos_authd_password password;
    struct leonos_auth_record *record;
    if (length < sizeof(password)) { authd_send_ack(slot, -1); return; }
    memcpy(&password, buffer, sizeof(password));
    if (!authd_text_valid(password.new_password, sizeof(password.new_password)) ||
        (clients[slot].uid != 0 && !authd_text_valid(password.old_password, sizeof(password.old_password)))) {
        memset(&password, 0, sizeof(password));
        authd_send_ack(slot, -1);
        return;
    }
    record = authd_find_uid(password.uid);
    if (!record || (clients[slot].uid != 0 && clients[slot].uid != password.uid)) {
        memset(password.old_password, 0, sizeof(password.old_password));
        memset(password.new_password, 0, sizeof(password.new_password));
        authd_send_ack(slot, -1);
        return;
    }
    if (clients[slot].uid != 0 && !authd_verify(record, password.old_password)) {
        memset(password.old_password, 0, sizeof(password.old_password));
        memset(password.new_password, 0, sizeof(password.new_password));
        authd_send_ack(slot, -1);
        return;
    }
    uint8_t previous_hash[LEONOS_AUTH_HASH_LEN];
    memcpy(previous_hash, record->password_hash, sizeof(previous_hash));
    if (authd_set_password(record, password.new_password) < 0) {
        explicit_bzero(previous_hash, sizeof(previous_hash));
        explicit_bzero(&password, sizeof(password));
        authd_send_ack(slot, -errno);
        return;
    }
    memset(password.old_password, 0, sizeof(password.old_password));
    memset(password.new_password, 0, sizeof(password.new_password));
    if (authd_save() < 0) {
        memcpy(record->password_hash, previous_hash, sizeof(previous_hash));
        explicit_bzero(previous_hash, sizeof(previous_hash));
        authd_send_ack(slot, -errno);
        return;
    }
    explicit_bzero(previous_hash, sizeof(previous_hash));
    /* The account's credentials just changed; any window opened with the old
     * password must not survive it. */
    (void)authd_sudo_cache_revoke_all();
    authd_send_ack(slot, 1);
}

static void authd_handle_power(int slot, const uint8_t *buffer, uint32_t length)
{
    struct leonos_authd_power request;
    struct authd_client *client = &clients[slot];
    struct leonos_auth_record *record = authd_find_uid(client->uid);
    /* Only root services or the enabled, currently logged-in console user.
     * The UID comes from SO_PEERCRED, never from the request or session file. */
    if (client->uid && (!session_active || client->uid != current_uid || !record ||
                       (record->user.flags & LEONOS_AUTH_USER_DISABLED))) {
        authd_send_ack(slot, -EPERM);
        return;
    }
    if (length != sizeof(request)) { authd_send_ack(slot, -EINVAL); return; }
    memcpy(&request, buffer, sizeof(request));
    if (request.reserved || (request.command != RB_AUTOBOOT && request.command != RB_POWER_OFF)) {
        authd_send_ack(slot, -EINVAL);
        return;
    }
    fprintf(stderr, "[authd.elf] power request uid=%u command=0x%x\n", client->uid, request.command);
    sync();
    int result = reboot((int)request.command);
    int error = result < 0 ? errno : EIO;
    authd_send_ack(slot, -error);
}

/* Serve one received frame. `stdio_fd` is the ancillary descriptor of that
 * frame (RUN only) and is consumed here on every path. Returns 0 to keep the
 * connection and -1 when the peer must be disconnected. */
static int authd_dispatch_message(int slot, uint32_t type, uint8_t *buffer,
                                  uint32_t length, int stdio_fd)
{
    struct authd_client *client = &clients[slot];
    pid_t sid = getsid((pid_t)client->pid);
    if (sid < 0) {
        if (stdio_fd >= 0) close(stdio_fd);
        return -1;
    }
    struct authd_sudo_channel channel = {
        .send = authd_sudo_channel_send,
        .context = (void *)(intptr_t)client->fd,
        .send_fd = authd_sudo_channel_send_fd,
        .owner_pid = client->pid,
        .session_id = (uint32_t)sid,
    };
    struct authd_sudo_records table = {.records = users, .count = user_count};
    int result = 0;

    if (type == LEONOS_AUTHD_MSG_HELLO) {
        struct leonos_authd_hello hello;
        if (length < sizeof(hello)) {
            result = -1;
        } else {
            memcpy(&hello, buffer, sizeof(hello));
            if (hello.pid != client->pid) {
                result = -1;
            } else {
                authd_send_ack(slot, 1);
            }
        }
    } else if (type == LEONOS_AUTHD_MSG_STATUS) {
        struct leonos_auth_status status = {0};
        authd_fill_status(&status);
        (void)leonos_ipc_send(client->fd, LEONOS_AUTHD_MSG_STATUS,
                              &status, sizeof(status));
    } else if (type == LEONOS_AUTHD_MSG_LIST) {
        struct leonos_authd_list request;
        struct leonos_authd_list_ack ack;
        uint8_t payload[AUTHD_FRAME_CAP];
        uint32_t offset = sizeof(ack);
        uint32_t count = 0;
        if (length >= sizeof(request)) {
            memcpy(&request, buffer, sizeof(request));
            memset(&ack, 0, sizeof(ack));
            for (uint32_t i = 0; i < user_count; ++i) {
                struct leonos_user_info *user = &users[i].user;
                if ((user->flags & LEONOS_AUTH_USER_DISABLED) && !request.include_disabled) continue;
                if (request.capacity > 0 && count < request.capacity &&
                    offset + sizeof(*user) <= sizeof(payload)) {
                    memcpy(payload + offset, user, sizeof(*user));
                    offset += sizeof(*user);
                }
                ++count;
            }
            ack.count = count;
            memcpy(payload, &ack, sizeof(ack));
            (void)leonos_ipc_send(client->fd, LEONOS_AUTHD_MSG_LIST, payload,
                                  offset);
        }
    } else if (type == LEONOS_AUTHD_MSG_LOGIN) {
        authd_handle_login(slot, buffer, length);
    } else if (type == LEONOS_AUTHD_MSG_ELEVATE) {
        authd_handle_elevate(slot, buffer, length);
    } else if (type == LEONOS_AUTHD_MSG_CURRENT) {
        struct leonos_auth_record *record;
        uint32_t uid = client->uid ? client->uid : current_uid;
        record = (client->uid || session_active) ? authd_find_uid(uid) : 0;
        if (record) {
            (void)leonos_ipc_send(client->fd, LEONOS_AUTHD_MSG_CURRENT,
                                  &record->user, sizeof(record->user));
        } else {
            authd_send_ack(slot, -1);
        }
    } else if (type == LEONOS_AUTHD_MSG_LOGOUT) {
        if (client->uid == 0 || client->uid == current_uid) {
            if (authd_publish_session(AUTHD_SESSION_FILE, 0) < 0) {
                /* Report the failure but keep the session state intact, so a
                 * transient write error cannot silently log the console out. */
                authd_send_ack(slot, -errno);
                if (stdio_fd >= 0) close(stdio_fd);
                return 0;
            }
            current_uid = 0;
            session_active = 0;
        }
        authd_send_ack(slot, 0);
    } else if (type == LEONOS_AUTHD_MSG_CREATE) {
        authd_handle_create(slot, buffer, length);
    } else if (type == LEONOS_AUTHD_MSG_UPDATE) {
        authd_handle_update(slot, buffer, length);
    } else if (type == LEONOS_AUTHD_MSG_CHANGE_PASSWORD) {
        authd_handle_password(slot, buffer, length);
    } else if (type == LEONOS_AUTHD_MSG_POWER) {
        authd_handle_power(slot, buffer, length);
    } else if (type == LEONOS_AUTHD_MSG_RUN_FD) {
        struct leonos_authd_run_fd descriptor;
        if (length != sizeof(descriptor)) result = -1;
        else {
            memcpy(&descriptor, buffer, sizeof(descriptor));
            if (descriptor.index >= 3 || (client->stdio_mask & (1u << descriptor.index))) result = -1;
            else {
                client->stdio[descriptor.index] = stdio_fd;
                client->stdio_mask |= 1u << descriptor.index;
                stdio_fd = -1;
            }
        }
    } else if (type == LEONOS_AUTHD_MSG_RUN) {
        struct authd_run_context run = {.request = NULL};
        if (client->stdio_mask != 7 || stdio_fd >= 0) {
            result = -1;
        } else {
            memcpy(run.stdio, client->stdio, sizeof(run.stdio));
            result = authd_sudo_run_from_peer(client->uid, buffer, length, stdio_fd,
                                            &channel, &table, authd_sudo_spawn,
                                            authd_sudo_verify, &run, &table);
            for (unsigned i = 0; i < 3; ++i) {
                if (client->stdio[i] >= 0) close(client->stdio[i]);
                client->stdio[i] = -1;
            }
            client->stdio_mask = 0;
            stdio_fd = -1;
        }
    } else if (type == LEONOS_AUTHD_MSG_WAIT) {
        authd_sudo_wait_from_peer(client->uid, buffer, length, &channel);
    } else if (type == LEONOS_AUTHD_MSG_RUN_SIGNAL) {
        authd_sudo_signal_from_peer(client->uid, buffer, length, &channel);
    } else if (type == LEONOS_AUTHD_MSG_SUDO_KILL) {
        authd_sudo_kill_from_peer(client->uid, &channel);
    } else if (type == LEONOS_AUTHD_MSG_SUDO_CHECK) {
        authd_sudo_check_from_peer(client->uid, &channel);
    } else if (type == LEONOS_AUTHD_MSG_SUDO_VERIFY) {
        authd_sudo_verify_from_peer(client->uid, buffer, length, &channel,
                                    &table, authd_sudo_verify, &table);
    } else if (type == LEONOS_AUTHD_MSG_FILEOP) {
        authd_sudo_fileop_from_peer(client->uid, buffer, length, &channel,
                                    &table, authd_sudo_spawn, authd_sudo_verify,
                                    NULL, &table);
    } else {
        /* Unknown message on a fixed protocol: do not guess, drop the peer. */
        result = -1;
    }
    /* RUN consumes stdio_fd inside its handler (the child duplicated it into
     * its own stdio), so this only ever fires for a rejected frame. */
    if (stdio_fd >= 0) close(stdio_fd);
    return result;
}

static void authd_drop_client(struct authd_client *client)
{
    for (unsigned i = 0; i < 3; ++i)
        if ((client->stdio_mask & (1u << i)) && client->stdio[i] >= 0)
            close(client->stdio[i]);
    leonos_ipc_close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static void authd_handle_client(int slot)
{
    struct authd_client *client = &clients[slot];
    uint8_t buffer[AUTHD_FRAME_CAP];
    uint32_t type = 0;
    uint32_t length = 0;
    for (;;) {
        struct pollfd descriptor = {.fd = client->fd, .events = POLLIN, .revents = 0};
        int stdio_fd = -1;
        if (poll(&descriptor, 1, 0) <= 0) return;
        /* RUN carries the caller's terminal descriptor, so the receive must
         * keep the ancillary fd instead of discarding it. */
        struct ucred expected = {(pid_t)client->pid, client->uid, client->gid};
        if (leonos_ipc_recv_cred_fd(client->fd, &type, buffer, sizeof(buffer), &length,
                                    &stdio_fd, &expected) < 0) {
            explicit_bzero(buffer, sizeof(buffer));
            if (errno == EAGAIN) return;
            if (stdio_fd >= 0) close(stdio_fd);
            authd_drop_client(client);
            return;
        }
        int result = authd_dispatch_message(slot, type, buffer, length, stdio_fd);
        explicit_bzero(buffer, sizeof(buffer));
        if (result < 0) {
            authd_drop_client(client);
            return;
        }
    }
}

int main(void)
{
    printf("[authd.elf] starting pid=%d uid=%d\n", getpid(), getuid());
    memset(users, 0, sizeof(users));
    memset(clients, 0, sizeof(clients));
    for (uint32_t i = 0; i < AUTHD_MAX_CLIENTS; ++i) clients[i].fd = -1;
    /* /run is currently on the system filesystem. A previous boot's session
     * must not make the launcher drop uid before starting the login app. */
    if (unlink(AUTHD_SESSION_FILE) < 0 && errno != ENOENT) {
        printf("[authd.elf] reset session state failed errno=%d\n", errno);
        return 1;
    }
    (void)mkdir("/run", 0755);
    (void)mkdir("/run/leonos", 0755);
    int loaded = authd_load();
    if (loaded != 0) {
        if (loaded < 0 && errno != ENOENT) {
            printf("[authd.elf] users.db read failed errno=%d\n", errno);
            return 1;
        }
        user_count = 0;
        if (authd_save() < 0) return 1;
    }
    if (authd_export_accounts("/etc", users, user_count) < 0) {
        printf("[authd.elf] passwd/group export failed errno=%d\n", errno);
        return 1;
    }
    printf("[authd.elf] users.db loaded count=%u current_uid=%u\n",
           user_count, current_uid);
    listen_fd = leonos_ipc_bind_listen_mode(LEONOS_IPC_SOCK_AUTH, 8, 0666);
    if (listen_fd < 0) {
        printf("[authd.elf] bind failed errno=%d\n", errno);
        return 1;
    }
    int passcred = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) < 0) {
        printf("[authd.elf] enable message credentials failed errno=%d\n", errno);
        close(listen_fd);
        return 1;
    }
    (void)leonos_ipc_set_nonblock(listen_fd, 1);
    printf("[authd.elf] listening on %s\n", LEONOS_IPC_SOCK_AUTH);
    for (;;) {
        struct pollfd descriptor = {.fd = listen_fd, .events = POLLIN, .revents = 0};
        if (poll(&descriptor, 1, 4) > 0 && (descriptor.revents & POLLIN)) {
            int fd;
            while ((fd = leonos_ipc_accept(listen_fd, 0)) >= 0) {
                struct ucred credentials;
                int slot = -1;
                for (uint32_t i = 0; i < AUTHD_MAX_CLIENTS; ++i) {
                    if (!clients[i].used) { slot = (int)i; break; }
                }
                if (slot < 0 || leonos_ipc_peer_credentials(fd, &credentials) < 0 ||
                    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) < 0) {
                    close(fd);
                    continue;
                }
                (void)leonos_ipc_set_nonblock(fd, 1);
                clients[slot].used = 1;
                clients[slot].fd = fd;
                clients[slot].pid = (uint32_t)credentials.pid;
                clients[slot].uid = credentials.uid;
                clients[slot].gid = credentials.gid;
                printf("[authd.elf] client pid=%u uid=%u\n",
                       clients[slot].pid, clients[slot].uid);
            }
        }
        for (uint32_t i = 0; i < AUTHD_MAX_CLIENTS; ++i) {
            if (clients[i].used) authd_handle_client(i);
        }
        /* Reap privileged children and release unclaimed result slots so a
         * long session cannot accumulate zombies or stale brokered requests. */
        authd_sudo_maintenance();
    }
}
