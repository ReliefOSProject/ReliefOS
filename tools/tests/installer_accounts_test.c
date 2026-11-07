#include <assert.h>
#include "../../kernel/reliefnt/tools/tests/host_tmp.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "legacy_authd/accounts.h"

int main(void)
{
    char directory[192];
    host_tmp_path(directory, sizeof(directory), "installer-accounts-XXXXXX");
    assert(mkdtemp(directory));
    struct reliefos_auth_record records[2] = {
        {.user = {.uid = 0, .role = 2, .username = "root", .home = "/root"}},
        {.user = {.uid = 1000, .role = 1, .username = "alice", .home = "/home/alice"}},
    };
    assert(authd_export_accounts(directory, records, 2) == 0);
    char path[256], text[2048];
    snprintf(path, sizeof(path), "%s/passwd", directory);
    FILE *file = fopen(path, "r");
    assert(file);
    size_t size = fread(text, 1, sizeof(text) - 1, file);
    text[size] = 0;
    fclose(file);
    assert(strstr(text, "root:x:0:0::/root:/bin/sh\n"));
    assert(strstr(text, "alice:x:1000:1000::/home/alice:/bin/sh\n"));
    assert(!strstr(text, "\nroot:"));
    assert(authd_set_password(&records[0], "") == -1 && errno == EINVAL);
    assert(!authd_check_password(&records[0], ""));
    assert(authd_set_password(&records[0], "r") == 0);
    assert(authd_check_password(&records[0], "r"));
    assert(!authd_check_password(&records[0], "wrong"));
    assert(authd_set_password(&records[1], "a b") == -1 && errno == EINVAL);
    assert(authd_set_password(&records[1], "a:@!\\\"$") == 0);
    assert(authd_check_password(&records[1], "a:@!\\\"$"));
    assert(!authd_check_password(&records[1], "a"));
    char long_password[33];
    memset(long_password, 'x', sizeof(long_password) - 1);
    long_password[sizeof(long_password) - 1] = 0;
    assert(authd_set_password(&records[1], long_password) == 0);
    assert(authd_check_password(&records[1], long_password));
    long_password[31] = 'y';
    assert(!authd_check_password(&records[1], long_password));
    const char *reference = getenv("LEONOS_TEST_PASSWORD_HASH");
    assert(reference && strlen(reference) < sizeof(records[1].password_hash));
    strcpy((char *)records[1].password_hash, reference);
    assert(authd_check_password(&records[1], "reference-password"));
    records[1].password_hash[22] = '!';
    assert(!authd_check_password(&records[1], "reference-password"));
    assert(!reliefos_auth_password_valid("123456789012345678901234567890123", 34));
    assert(!reliefos_auth_password_valid("before after", 13));
    assert(!reliefos_auth_password_valid("before\tafter", 13));
    assert(!reliefos_auth_password_valid("\xe3\x80\x80", 4));
    char unicode_password[133];
    for (unsigned i = 0; i < 33; ++i) memcpy(unicode_password + i * 4, "\xf0\x9f\x94\x91", 4);
    unicode_password[132] = 0;
    assert(!reliefos_auth_password_valid(unicode_password, sizeof(unicode_password)));
    unicode_password[128] = 0;
    assert(reliefos_auth_password_valid(unicode_password, sizeof(unicode_password)));
    assert(authd_set_password(&records[1], unicode_password) == 0);
    assert(authd_check_password(&records[1], unicode_password));
    assert(!reliefos_auth_password_valid("\xc0\xaf", 3));
    assert(!reliefos_auth_password_valid("\xed\xa0\x80", 4));
    assert(!reliefos_auth_password_valid("\xf0\x9f", 3));
    records[1].user.uid = 0;
    assert(authd_export_accounts(directory, records, 2) == -1 && errno == EINVAL);
    records[1].user.uid = 1000;
    records[1].user.role = 2;
    assert(authd_export_accounts(directory, records, 2) == -1 && errno == EINVAL);
    unlink(path);
    snprintf(path, sizeof(path), "%s/group", directory);
    unlink(path);
    rmdir(directory);
    puts("installer accounts: root and ordinary identity contracts PASS");
    return 0;
}
