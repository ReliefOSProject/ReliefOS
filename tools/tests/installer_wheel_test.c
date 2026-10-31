#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <gshadow.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../userland/auth/standard_accounts.h"

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
    puts("installer groups: PASS video/wheel membership, gshadow, nonroot identity and reserved names");
    return 0;
}
