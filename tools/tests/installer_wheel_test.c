#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <gshadow.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../userland/auth/standard_accounts.h"

static void write_fixture(const char *target, const char *name, const char *contents)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/etc/%s", target, name);
    FILE *file = fopen(path, "w");
    assert(file && fputs(contents, file) != EOF && fclose(file) == 0);
}

static void expect_line(const char *target, const char *name, const char *expected)
{
    char path[256], line[1024];
    snprintf(path, sizeof(path), "%s/etc/%s", target, name);
    FILE *file = fopen(path, "r");
    assert(file);
    unsigned matches = 0;
    while (fgets(line, sizeof(line), file)) matches += !strcmp(line, expected);
    assert(matches == 1 && fclose(file) == 0);
}

static void check_packaged_accounts(void)
{
    char target[] = "/tmp/installer-packaged-XXXXXX", path[256];
    assert(mkdtemp(target));
    snprintf(path, sizeof(path), "%s/etc", target);
    assert(mkdir(path, 0700) == 0);
    write_fixture(target, "passwd", "root:x:0:0:root:/root:/bin/bash\n"
                  "messagebus:x:999:999::/dev/null:/sbin/nologin\n");
    write_fixture(target, "shadow", "root:!:::::::\nmessagebus:!:::::::\n");
    write_fixture(target, "group", "root:x:0:\nwheel:x:10:root\n"
                  "messagebus:x:999:\npolkitd:x:998:");
    write_fixture(target, "gshadow", "root:!::\nwheel:!:root:root\n"
                  "messagebus:!::\npolkitd:!::\n");
    assert(reliefos_account_seed_check(target, "messagebus") == -1 && errno == EEXIST);
    assert(reliefos_account_seed_check(target, "polkitd") == -1 && errno == EEXIST);
    assert(reliefos_account_seed_check(target, "alice") == 0);
    assert(reliefos_account_seed(target, "alice", "user-password", "root-password") == 0);
    expect_line(target, "passwd", "messagebus:x:999:999::/dev/null:/sbin/nologin\n");
    expect_line(target, "shadow", "messagebus:!:::::::\n");
    expect_line(target, "group", "messagebus:x:999:\n");
    expect_line(target, "group", "polkitd:x:998:\n");
    expect_line(target, "group", "wheel:x:10:root,alice\n");
    expect_line(target, "gshadow", "polkitd:!::\n");
    expect_line(target, "gshadow", "wheel:!:root:root,alice\n");
    snprintf(path, sizeof(path), "%s/etc/shadow", target);
    FILE *file = fopen(path, "r");
    assert(file);
    struct spwd *entry;
    unsigned credentials = 0;
    while ((entry = fgetspent(file))) {
        if (!strcmp(entry->sp_namp, "root") || !strcmp(entry->sp_namp, "alice")) {
            assert(!strncmp(entry->sp_pwdp, "$y$", 3));
            ++credentials;
        }
    }
    assert(credentials == 2 && fclose(file) == 0);

    assert(reliefos_account_seed(target, "messagebus", "password", "password") == -1 && errno == EEXIST);
    write_fixture(target, "passwd", "other:x:1000:999::/dev/null:/sbin/nologin\n");
    assert(reliefos_account_seed(target, "bob", "password", "password") == -1 && errno == EEXIST);
    expect_line(target, "passwd", "other:x:1000:999::/dev/null:/sbin/nologin\n");
}

int main(void)
{
    char target[] = "/tmp/installer-wheel-XXXXXX", path[256];
    assert(mkdtemp(target));
    snprintf(path, sizeof(path), "%s/etc", target);
    assert(mkdir(path, 0700) == 0);
    assert(reliefos_account_seed(target, "alice", "user-password", "root-password") == 0);

    snprintf(path, sizeof(path), "%s/etc/group", target);
    FILE *file = fopen(path, "r");
    assert(file);
    struct group *group;
    unsigned wheel = 0, video = 0, personal = 0;
    while ((group = fgetgrent(file))) {
        if (!strcmp(group->gr_name, "wheel")) {
            ++wheel;
            assert(group->gr_gid == 10 && group->gr_mem[0]);
            assert(!strcmp(group->gr_mem[0], "alice") && !group->gr_mem[1]);
        }
        if (!strcmp(group->gr_name, "alice")) {
            ++personal;
            assert(group->gr_gid == 1000);
        }
        if (!strcmp(group->gr_name, "video")) {
            ++video;
            assert(group->gr_gid == 44 && group->gr_mem[0]);
            assert(!strcmp(group->gr_mem[0], "alice") && !group->gr_mem[1]);
        }
    }
    assert(wheel == 1 && video == 1 && personal == 1);
    fclose(file);

    snprintf(path, sizeof(path), "%s/etc/gshadow", target);
    file = fopen(path, "r");
    assert(file);
    struct sgrp *shadow;
    wheel = video = 0;
    while ((shadow = fgetsgent(file))) {
      if (!strcmp(shadow->sg_namp, "wheel")) {
        ++wheel;
        assert(!strcmp(shadow->sg_passwd, "!"));
        assert(!shadow->sg_adm[0] && shadow->sg_mem[0]);
        assert(!strcmp(shadow->sg_mem[0], "alice") && !shadow->sg_mem[1]);
      }
      if (!strcmp(shadow->sg_namp, "video")) {
        ++video;
        assert(!strcmp(shadow->sg_passwd, "!"));
        assert(!shadow->sg_adm[0] && shadow->sg_mem[0]);
        assert(!strcmp(shadow->sg_mem[0], "alice") && !shadow->sg_mem[1]);
      }
    }
    assert(wheel == 1 && video == 1);
    fclose(file);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_uid == 0 && (st.st_mode & 0777) == 0600);

    snprintf(path, sizeof(path), "%s/etc/passwd", target);
    file = fopen(path, "r");
    assert(file);
    struct passwd *account;
    unsigned users = 0;
    while ((account = fgetpwent(file))) if (!strcmp(account->pw_name, "alice")) {
        ++users;
        assert(account->pw_uid == 1000 && account->pw_gid == 1000);
    }
    assert(users == 1);
    fclose(file);
    assert(!reliefos_account_name_valid("alice,bob", 32));
    assert(!reliefos_account_name_valid("video", 32));
    errno = 0;
    assert(reliefos_account_seed(target, "wheel", "password", "password") == -1 && errno == EINVAL);
    errno = 0;
    assert(reliefos_account_seed(target, "video", "password", "password") == -1 && errno == EINVAL);
    check_packaged_accounts();
    puts("installer accounts: PASS identities, credentials, group membership and packaged accounts");
    return 0;
}
