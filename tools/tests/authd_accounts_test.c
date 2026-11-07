#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../userland/apps/authd/accounts.h"

int main(void)
{
    char directory[192], path[256];
    host_tmp_path(directory, sizeof(directory), "accounts-XXXXXX");
    assert(mkdtemp(directory));
    struct reliefos_auth_record records[2] = {0};
    records[0].user.uid = 0;
    records[0].user.role = RELIEFOS_AUTH_ROLE_ADMIN;
    strcpy(records[0].user.username, "root");
    strcpy(records[0].user.home, "/root");
    memset(records[0].password_hash, 'Z', sizeof(records[0].password_hash));
    records[1].user.uid = 1000;
    records[1].user.role = RELIEFOS_AUTH_ROLE_USER;
    strcpy(records[1].user.username, "second-user");
    strcpy(records[1].user.home, "/home/second-user");
    assert(authd_export_accounts(directory, records, 2) == 0);
    snprintf(path, sizeof(path), "%s/passwd", directory);
    FILE *file = fopen(path, "r");
    assert(file);
    struct passwd *user = fgetpwent(file);
    assert(user && !strcmp(user->pw_name, "root") && user->pw_uid == 0 && user->pw_gid == 0);
    assert(!strcmp(user->pw_passwd, "x") && !strcmp(user->pw_dir, "/root"));
    assert(!strcmp(user->pw_shell, "/bin/sh"));
    user = fgetpwent(file);
    assert(user && user->pw_uid == 65534 && !strcmp(user->pw_name, "nobody"));
    user = fgetpwent(file);
    assert(user && user->pw_uid == 1000 && user->pw_gid == 1000);
    assert(!fgetpwent(file));
    fclose(file);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644);
    snprintf(path, sizeof(path), "%s/group", directory);
    file = fopen(path, "r");
    assert(file);
    struct group *group = fgetgrent(file);
    assert(group && group->gr_gid == 0 && !strcmp(group->gr_name, "root"));
    assert(group->gr_mem && !group->gr_mem[0]);
    group = fgetgrent(file);
    assert(group && group->gr_gid == 65534 && !strcmp(group->gr_name, "nobody"));
    group = fgetgrent(file);
    assert(group && group->gr_gid == 1000 && !strcmp(group->gr_name, "second-user"));
    assert(!fgetgrent(file));
    fclose(file);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644);

    /* Reject record injection before changing either published file. */
    strcpy(records[1].user.username, "bad:name");
    assert(authd_export_accounts(directory, records, 2) == -1 && errno == EINVAL);
    strcpy(records[1].user.username, "second-user");
    strcpy(records[1].user.home, "/home/second-user\nextra:x:0:0");
    assert(authd_export_accounts(directory, records, 2) == -1 && errno == EINVAL);
    assert(!authd_username_valid("..", 3));
    assert(!authd_username_valid("../other", 9));
    assert(!authd_username_valid("a\nb", 4));
    char unterminated[32];
    memset(unterminated, 'a', sizeof(unterminated));
    assert(!authd_username_valid(unterminated, sizeof(unterminated)));
    assert(authd_username_valid("normal-user_2", 32));

    /* Successful replacement removes stale records and retains public mode. */
    assert(authd_export_accounts(directory, records, 1) == 0);
    file = fopen(path, "r");
    assert(file && fgetgrent(file) && fgetgrent(file) && !fgetgrent(file));
    fclose(file);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/passwd", directory);
    assert(unlink(path) == 0);
    assert(authd_export_accounts("/proc/reliefos-accounts-test", records, 1) == -1);
    assert(rmdir(directory) == 0);
    puts("accounts: standard passwd/group lookup, UID preservation, modes and invalid records PASS");
    return 0;
}
