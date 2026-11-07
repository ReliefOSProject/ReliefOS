#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <reliefos/auth.h>

static void write_file(const char *path, const char *contents, mode_t mode)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(contents, file) >= 0 && fclose(file) == 0);
    assert(chmod(path, mode) == 0);
}

int execv(const char *path, char *const argv[])
{
    FILE *log = fopen("/tmp/account-commands.log", "a");
    assert(log);
    for (size_t i = 0; argv[i]; ++i) assert(fprintf(log, "<%s>\n", argv[i]) > 0);
    assert(fputs("--\n", log) >= 0 && fclose(log) == 0);

    if (!strcmp(path, "/usr/sbin/useradd")) {
        write_file("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n"
                   "alice:x:1000:1000::/home/alice:/bin/bash\n", 0644);
        write_file("/etc/shadow", "root:!:20000:0:99999:7:::\n"
                   "alice:x:20000:0:99999:7:::\n", 0600);
    }
    _exit(0);
}

int main(void)
{
    assert(mkdir("/run", 0755) == 0 || errno == EEXIST);
    assert(mkdir("/etc", 0755) == 0 || errno == EEXIST);
    write_file("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n", 0644);
    write_file("/etc/shadow", "root:!:20000:0:99999:7:::\n", 0600);

    struct reliefos_user_info user;
    errno = 0;
    assert(reliefos_auth_create_user("video", "AbC123!", RELIEFOS_AUTH_ROLE_USER,
                                     &user) == -1 && errno == EINVAL);
    assert(access("/tmp/account-commands.log", F_OK) == -1 && errno == ENOENT);
    assert(reliefos_auth_create_user("alice", "AbC123!", RELIEFOS_AUTH_ROLE_USER,
                                     &user) == 0);
    assert(user.uid == 1000 && !strcmp(user.username, "alice"));

    FILE *file = fopen("/tmp/account-commands.log", "r");
    assert(file);
    char log[2048] = {0};
    assert(fread(log, 1, sizeof(log) - 1, file) > 0);
    assert(fclose(file) == 0);
    assert(strstr(log, "</usr/sbin/useradd>\n<-m>\n<-U>\n<-s>\n</bin/bash>\n"
                       "<-G>\n<video>\n<-e>\n<1970-01-02>\n<-->\n<alice>\n"));
    assert(strstr(log, "</usr/sbin/chpasswd>\n--\n"));
    assert(strstr(log, "</usr/sbin/usermod>\n<-e>\n<>\n<-->\n<alice>\n"));
    puts("normal user creation: PASS useradd adds the video group and completes account setup");
    return 0;
}
