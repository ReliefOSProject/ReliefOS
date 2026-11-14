#define _POSIX_C_SOURCE 200809L
#include "installer_update.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *root, const char *relative, const char *text)
{
    char path[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/%s", root, relative) < (int)sizeof(path));
    FILE *stream = fopen(path, "w");
    assert(stream);
    assert(fputs(text, stream) >= 0);
    assert(!fclose(stream));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *root = argv[1];
    const char *const directories[] = {
        "etc", "etc/apk", "etc/reliefos", "lib", "lib/apk", "lib/apk/db",
        "var", "var/lib", "usr", "usr/bin", "usr/lib", "usr/share", "bin", "sbin", "boot",
    };
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s", root, directories[i]) < (int)sizeof(path));
        assert(!mkdir(path, 0755));
    }
    write_file(root, "etc/os-release", "ID=reliefos\nVERSION_ID=5.3.1\n");
    write_file(root, "etc/reliefos/desktop-backend", "console\n");
    struct installer_update_info info = {0};
    assert(!installer_update_read_system(root, &info));
    assert(!strcmp(info.installed_version, "5.3.1"));
    assert(!strcmp(info.profile, "console"));
    write_file(root, "etc/reliefos/desktop-backend", "console\nxorg\n");
    assert(installer_update_read_system(root, &info) == -EINVAL);
    write_file(root, "etc/reliefos/desktop-backend", "xorg\n");
    write_file(root, "etc/os-release", "ID=reliefos\nVERSION_ID=\"$(touch /tmp/unwanted)\"\n");
    assert(installer_update_read_system(root, &info) == -EINVAL);
    write_file(root, "usr/lib/os-release", "ID=reliefos\nVERSION_ID=\"5.3.1\"\n");
    snprintf(path, sizeof(path), "%s/etc/os-release", root);
    assert(!unlink(path));
    assert(!symlink("../usr/lib/os-release", path));
    assert(!installer_update_read_system(root, &info));
    assert(!strcmp(info.profile, "xorg"));
    assert(!unlink(path));
    assert(!symlink("/etc/os-release", path));
    assert(installer_update_read_system(root, &info) == -EINVAL);

    write_file(root, "lib/apk/db/installed",
               "P:external-fixture\nV:1.0-r0\nI:999999\n\n"
               "P:reliefos-base\nV:5.3.1-r1\nI:100\n\n"
               "P:reliefos-kernel\nV:5.3.1-r2\nI:200\n\n");
    struct installer_update_packages packages;
    assert(!installer_update_read_packages(root, &packages));
    assert(packages.installed_bytes == 300 && packages.largest_package == 200);
    assert(!strcmp(packages.kernel_version, "5.3.1-r2"));

    unsigned progress = 0;
    char message[128] = "";
    assert(installer_update_decode(&info, &progress, message, sizeof(message),
                                   "progress\t40\tApplying signed packages") == 2);
    assert(progress == 40 && !strcmp(message, "Applying signed packages"));
    assert(installer_update_decode(&info, &progress, message, sizeof(message),
                                   "progress\t101\tInvalid") == -EINVAL);
    assert(installer_update_decode(&info, &progress, message, sizeof(message),
                                   "info\tfree_bytes\t18446744073709551615") == 1);
    assert(info.free_bytes == UINT64_MAX);
    assert(installer_update_decode(&info, &progress, message, sizeof(message),
                                   "info\tfree_bytes\t18446744073709551616") == -EINVAL);

    assert(installer_update_decode(&info, &progress, message, sizeof(message),
        "activity\tcache\treused\treliefos-test-1.apk\t1\t2\t1048576\t2097152\t1048576\t0") == 4);
    assert(info.activity.bytes_done == 1048576 && info.activity.bytes_reused == 1048576);
    assert(installer_update_activity_percent(&info.activity, 1) == 68);
    char encoded[512], detail[768];
    assert(!installer_update_activity_encode(&info.activity, encoded, sizeof(encoded)));
    struct installer_update_activity original = info.activity, decoded = {0};
    assert(installer_update_activity_decode(&decoded, encoded) == 1);
    assert(!memcmp(&original, &decoded, sizeof(original)));
    assert(!installer_update_activity_format(&decoded, detail, sizeof(detail)));
    assert(strstr(detail, "reliefos-test-1.apk") && strstr(detail, "1.0/2.0 MiB"));
    assert(installer_update_activity_format(&decoded, detail, 1) == -EOVERFLOW);
    const char *const malformed[] = {
        "activity\tcache\treused\t../escape.apk\t1\t2\t1\t2\t1\t0",
        "activity\tunknown\treused\tp.apk\t1\t2\t1\t2\t1\t0",
        "activity\tcache\tunknown\tp.apk\t1\t2\t1\t2\t1\t0",
        "activity\tcache\treused\tp.apk\t3\t2\t1\t2\t1\t0",
        "activity\tcache\treused\tp.apk\t1\t2\t3\t2\t1\t0",
        "activity\tcache\treused\tp.apk\t1\t2\t1\t2\t2\t0",
        "activity\tcache\tfailed\tp.apk\t1\t2\t1\t2\t1\t0",
        "activity\tcache\treused\tp.apk\t1\t2\t1\t2\t1\t1",
        "activity\tcache\treused\tp.apk\t1\t2\t1\t18446744073709551616\t1\t0",
        "activity\tcache\treused\tp.apk\t1\t2\t1\t2\t1\t0\textra",
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        decoded = original;
        assert(installer_update_activity_decode(&decoded, malformed[i]) == -EINVAL);
        assert(!memcmp(&original, &decoded, sizeof(original)));
    }
    decoded = (struct installer_update_activity){.step = UPDATE_STEP_UPGRADE};
    assert(installer_update_activity_apk(&decoded, "(2/3) Upgrading reliefos-fixture (1 -> 2)") == 1);
    assert(decoded.files_done == 1 && decoded.files_total == 3);
    assert(!strcmp(decoded.package, "reliefos-fixture"));
    assert(!installer_update_activity_apk(&decoded, "(4/3) Upgrading reliefos-fixture (1 -> 2)"));
    assert(!installer_update_activity_apk(&decoded, "(1/3) NOTICE reliefos-fixture"));
    assert(installer_update_decode(&info, &progress, message, sizeof(message),
                                   "progress\t78\tSynchronizing") == 2);
    assert(!info.activity.step);

    struct installer_update_checkpoint state = {
        .phase = "packages", .root_uuid = "8ab5d176-6479-4ec8-b382-531fca6ea662",
        .index_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .database_sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        .world_sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    }, restored;
    assert(!installer_update_read_checkpoint(root, &restored));
    assert(!restored.phase[0]);
    assert(!installer_update_write_checkpoint(root, &state));
    assert(!installer_update_read_checkpoint(root, &restored));
    assert(!strcmp(restored.phase, "packages"));
    assert(!strcmp(restored.root_uuid, state.root_uuid));
    snprintf(path, sizeof(path), "var/lib/reliefos/installer-update/.state-%ld", (long)getpid());
    write_file(root, path, "stale temporary from a previous boot\n");
    strcpy(state.phase, "boot");
    assert(!installer_update_write_checkpoint(root, &state));
    assert(!installer_update_read_checkpoint(root, &restored));
    assert(!strcmp(restored.phase, "boot"));
    puts("PASS installer update metadata, protocol and durable retry state");
    return 0;
}
