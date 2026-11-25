#define _POSIX_C_SOURCE 200809L
#include "installer_update.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

static void test_package_growth(const char *root)
{
    char media[PATH_MAX], path[PATH_MAX];
    assert(snprintf(media, sizeof(media), "%s/growth-media", root) < (int)sizeof(media));
    assert(!mkdir(media, 0755));
    const char *const directories[] = {"lib", "lib/apk", "lib/apk/db"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s", media, directories[i]) < (int)sizeof(path));
        assert(!mkdir(path, 0755));
    }
    /* Unchanged and shrinking packages, removed legacy wrappers and retained
     * user packages cannot cancel the space needed by a new upstream library.
     * Input order differs deliberately so matching requires package names. */
    write_file(root, "lib/apk/db/installed",
        "P:external-retained\nV:1\nI:4000000000\n\n"
        "P:reliefos-sqlite\nV:1\nI:1000000\n\n"
        "P:same-version\nV:2-r0\nI:500\n\n"
        "P:shrinking\nV:1\nI:50000000\n\n"
        "P:growing\nV:1\nI:100\n\n");
    write_file(media, "lib/apk/db/installed",
        "P:llvm22-libs\nV:22-r0\nI:190973792\n\n"
        "P:shrinking\nV:2\nI:10\n\n"
        "P:same-version\nV:2-r0\nI:500\n\n"
        "P:sqlite-libs\nV:3\nI:1640464\n\n"
        "P:growing\nV:2\nI:150\n");
    uint64_t growth = 0, largest = 0;
    assert(!installer_update_measure_growth(root, media, &growth, &largest));
    assert(growth == UINT64_C(190973792) + UINT64_C(1640464) + 50);
    assert(largest == UINT64_C(190973792));
    /* Already committed packages remain affordable on a repair/retry rather
     * than charging the entire installed system's bytes once again. */
    assert(!installer_update_measure_growth(media, media, &growth, &largest));
    assert(!growth && largest == UINT64_C(190973792));

    write_file(root, "lib/apk/db/installed", "");
    const char *const invalid[] = {
        "P:missing-size\nV:1\n\n",
        "P:duplicate-size\nV:1\nI:1\nI:2\n\n",
        "P:../invalid-name\nV:1\nI:1\n\n",
        "P:negative-size\nV:1\nI:-1\n\n",
        "P:overflow-size\nV:1\nI:18446744073709551616\n\n",
        "P:duplicate-name\nV:1\nI:1\n\nP:duplicate-name\nV:2\nI:2\n\n",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        write_file(media, "lib/apk/db/installed", invalid[i]);
        growth = largest = 42;
        assert(installer_update_measure_growth(root, media, &growth, &largest) == -EINVAL);
        assert(!growth && !largest);
    }
    /* Duplicate names on the installed side are equally ambiguous. */
    write_file(root, "lib/apk/db/installed", invalid[5]);
    write_file(media, "lib/apk/db/installed", "P:new\nV:1\nI:1\n\n");
    assert(installer_update_measure_growth(root, media, &growth, &largest) == -EINVAL);
    assert(!growth && !largest);
    write_file(root, "lib/apk/db/installed", "");
    write_file(media, "lib/apk/db/installed",
        "P:first\nV:1\nI:18446744073709551615\n\nP:second\nV:1\nI:1\n\n");
    assert(installer_update_measure_growth(root, media, &growth, &largest) == -EOVERFLOW);
    assert(!growth && !largest);

    assert(snprintf(path, sizeof(path), "%s/lib/apk/db/installed", media) < (int)sizeof(path));
    assert(!unlink(path));
    assert(!mkfifo(path, 0600));
    assert(installer_update_measure_growth(root, media, &growth, &largest) == -EINVAL);
    assert(!growth && !largest);
    assert(!unlink(path));
    assert(!symlink("/etc/passwd", path));
    assert(installer_update_measure_growth(root, media, &growth, &largest) == -ELOOP);
    assert(!growth && !largest);
    assert(!unlink(path));
    FILE *stream = fopen(path, "w");
    assert(stream);
    assert(fwrite("P:hidden\0name\nV:1\nI:1\n", 1, 22, stream) == 22);
    assert(!fclose(stream));
    assert(installer_update_measure_growth(root, media, &growth, &largest) == -EINVAL);
    assert(!growth && !largest);

    stream = fopen(path, "w");
    assert(stream);
    for (unsigned i = 0; i < 8192; ++i) assert(fprintf(stream, "P:package-%u\nV:1\nI:1\n\n", i) > 0);
    assert(!fclose(stream));
    assert(!installer_update_measure_growth(root, media, &growth, &largest));
    assert(growth == 8192 && largest == 1);
    stream = fopen(path, "a");
    assert(stream && fputs("P:one-too-many\nV:1\nI:1\n\n", stream) >= 0 && !fclose(stream));
    assert(installer_update_measure_growth(root, media, &growth, &largest) == -EOVERFLOW);
    assert(!growth && !largest);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    /* A FIFO regression must fail promptly rather than hanging the test job. */
    alarm(10);
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
    assert(!unlink(path));
    assert(!mkfifo(path, 0600));
    assert(installer_update_read_system(root, &info) == -EINVAL);
    assert(!unlink(path));
    write_file(root, "etc/os-release", "ID=reliefos\nVERSION_ID=5.3.1\nID=leonos\n");
    assert(installer_update_read_system(root, &info) == -EINVAL);
    write_file(root, "etc/os-release", "ID=reliefos\nVERSION_ID=5.3.1\n");
    snprintf(path, sizeof(path), "%s/etc/reliefos/desktop-backend", root);
    assert(!unlink(path));
    assert(!mkfifo(path, 0600));
    assert(installer_update_read_system(root, &info) == -EINVAL);
    assert(!unlink(path));
    write_file(root, "etc/reliefos/desktop-backend", "console\n");

    write_file(root, "lib/apk/db/installed",
               "P:external-fixture\nV:1.0-r0\nI:999999\n\n"
               "P:reliefos-base\nV:5.3.1-r1\nI:100\n\n"
               "P:reliefos-kernel\nV:5.3.1-r2\nI:200\n\n");
    struct installer_update_packages packages;
    assert(!installer_update_read_packages(root, &packages));
    assert(packages.installed_bytes == 300 && packages.largest_package == 200);
    assert(!strcmp(packages.kernel_version, "5.3.1-r2"));
    assert(!packages.has_xfce_session);
    write_file(root, "lib/apk/db/installed", "P:xfce4-session\nV:4.20.2-r0\nI:44032\n");
    assert(!installer_update_read_packages(root, &packages) && packages.has_xfce_session);
    snprintf(path, sizeof(path), "%s/lib/apk/db/installed", root);
    assert(!unlink(path));
    assert(!mkfifo(path, 0600));
    assert(installer_update_read_packages(root, &packages) == -EINVAL);
    assert(!unlink(path));
    int oversized = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(oversized >= 0 && !ftruncate(oversized, 64u * 1024u * 1024u + 1u));
    assert(!close(oversized));
    assert(installer_update_read_packages(root, &packages) == -EOVERFLOW);
    assert(!unlink(path));
    write_file(root, "lib/apk/db/installed", "P:reliefos-kernel\nV:5.3.1-r2\nI:200\n\n");

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
    strcpy(state.phase, "unknown");
    assert(installer_update_write_checkpoint(root, &state) == -EINVAL);
    assert(!installer_update_read_checkpoint(root, &restored));
    assert(!strcmp(restored.phase, "boot"));
    strcpy(state.phase, "boot");
    state.index_sha256[0] = 'z';
    assert(installer_update_write_checkpoint(root, &state) == -EINVAL);
    state.index_sha256[0] = 'a';
    strcpy(state.root_uuid, "00000000-0000-0000-0000-000000000000");
    assert(installer_update_write_checkpoint(root, &state) == -EINVAL);
    strcpy(state.root_uuid, "8ab5d176-6479-4ec8-b382-531fca6ea662");
    snprintf(path, sizeof(path), "%s/var/lib/reliefos/installer-update/state", root);
    assert(!unlink(path));
    assert(!mkfifo(path, 0600));
    assert(installer_update_read_checkpoint(root, &restored) == -EINVAL);
    assert(!restored.phase[0]);
    assert(!unlink(path));
    assert(!symlink("/etc/passwd", path));
    assert(installer_update_read_checkpoint(root, &restored) == -ELOOP);
    assert(!unlink(path));
    oversized = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(oversized >= 0 && !ftruncate(oversized, 1025));
    assert(!close(oversized));
    assert(installer_update_read_checkpoint(root, &restored) == -EOVERFLOW);
    assert(!unlink(path));
    char invalid[1024];
    snprintf(invalid, sizeof(invalid),
        "format=1\nphase=unknown\nroot_uuid=%s\nindex_sha256=%s\ndatabase_sha256=%s\nworld_sha256=%s\n",
        state.root_uuid, state.index_sha256, state.database_sha256, state.world_sha256);
    write_file(root, "var/lib/reliefos/installer-update/state", invalid);
    assert(installer_update_read_checkpoint(root, &restored) == -EINVAL);
    assert(!restored.phase[0]);
    assert(!installer_update_write_checkpoint(root, &state));
    FILE *duplicate = fopen(path, "a");
    assert(duplicate && fputs("phase=done\n", duplicate) >= 0 && !fclose(duplicate));
    assert(installer_update_read_checkpoint(root, &restored) == -EINVAL);
    assert(!restored.phase[0]);
    assert(!installer_update_write_checkpoint(root, &state));
    assert(!installer_update_read_checkpoint(root, &restored));
    assert(!strcmp(state.phase, restored.phase));
    assert(!strcmp(state.root_uuid, restored.root_uuid));
    assert(!strcmp(state.index_sha256, restored.index_sha256));
    assert(!strcmp(state.database_sha256, restored.database_sha256));
    assert(!strcmp(state.world_sha256, restored.world_sha256));
    test_package_growth(root);
    alarm(0);
    puts("PASS installer update metadata, conservative per-package space, protocol and durable retry state");
    return 0;
}
