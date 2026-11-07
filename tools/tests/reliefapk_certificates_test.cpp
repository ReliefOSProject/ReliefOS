#include "certificates.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ra::Text read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    char *bytes = static_cast<char *>(malloc(static_cast<size_t>(length)));
    assert(bytes && fread(bytes, 1, static_cast<size_t>(length), file) == static_cast<size_t>(length));
    assert(fclose(file) == 0);
    ra::Text result;
    assert(result.set(bytes, static_cast<size_t>(length)));
    free(bytes);
    return result;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const int dir = open(argv[1], O_RDONLY | O_DIRECTORY);
    assert(dir >= 0);
    const ra::Text bundle = read_file(argv[2]);
    ra::Text backup, error;
    const char *name = "ca-certificates.crt";
    assert(ra::restore_ca_bundle_at(dir, bundle, backup, error));
    assert(backup.empty());
    assert(ra::safe_ca_bundle_at(dir, geteuid(), error));
    assert(!ra::safe_ca_bundle_at(dir, static_cast<unsigned long>(geteuid()) + 1, error));
    assert(fchmodat(dir, "ca-certificates.crt", 0666, 0) == 0);
    assert(!ra::safe_ca_bundle_at(dir, geteuid(), error));
    assert(fchmodat(dir, "ca-certificates.crt", 0644, 0) == 0);
    int fd = openat(dir, name, O_RDONLY);
    assert(fd >= 0);
    struct stat info;
    assert(fstat(fd, &info) == 0 && (info.st_mode & 0777) == 0644);
    char bytes[4096];
    size_t used = 0;
    ssize_t got;
    while ((got = read(fd, bytes, sizeof(bytes))) > 0) {
        assert(used + static_cast<size_t>(got) <= bundle.size());
        assert(!memcmp(bytes, bundle.c_str() + used, static_cast<size_t>(got)));
        used += static_cast<size_t>(got);
    }
    assert(got == 0 && used == bundle.size());
    close(fd);
    fd = openat(dir, name, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, "broken", 6) == 6 && close(fd) == 0);
    assert(ra::restore_ca_bundle_at(dir, bundle, backup, error));
    assert(!backup.empty());
    fd = openat(dir, backup.c_str(), O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0 && fstat(fd, &info) == 0 && (info.st_mode & 0777) == 0600);
    char saved[8] = {};
    assert(read(fd, saved, sizeof(saved)) == 6 && !strcmp(saved, "broken"));
    close(fd);
    assert(unlinkat(dir, name, 0) == 0 && symlinkat(backup.c_str(), dir, name) == 0);
    assert(!ra::safe_ca_bundle_at(dir, geteuid(), error));
    assert(!ra::restore_ca_bundle_at(dir, bundle, backup, error));
    assert(fstatat(dir, name, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(info.st_mode));
    assert(unlinkat(dir, name, 0) == 0);
    assert(ra::safe_ca_bundle_at(dir, geteuid(), error));
    ra::Text invalid;
    assert(invalid.set("not a certificate"));
    assert(!ra::restore_ca_bundle_at(dir, invalid, backup, error));
    assert(fstatat(dir, name, &info, AT_SYMLINK_NOFOLLOW) != 0);
    assert(fchmod(dir, 0777) == 0);
    assert(!ra::restore_ca_bundle_at(dir, bundle, backup, error));
    assert(fchmod(dir, 0700) == 0);
    const char *output_name = "apk-output";
    int output = openat(dir, output_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(output >= 0);
    int input[2];
    assert(pipe(input) == 0);
    const char *diagnostic = "(1/2) Reinstalling ca-certificates\n"
        "WARNING: fetching https://user:fixture-secret@example.test/packages.adb: TLS error\n"
        "(2/2) [APK unavailable, skipped] Reinstalling ca-certificates-bundle\nOK: done\n";
    assert(write(input[1], diagnostic, strlen(diagnostic)) == static_cast<ssize_t>(strlen(diagnostic)));
    assert(close(input[1]) == 0);
    const int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(output, STDOUT_FILENO) >= 0);
    const bool completed = ra::relay_certificate_apk_output(input[0]);
    assert(fflush(stdout) == 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    close(input[0]);
    assert(!completed && lseek(output, 0, SEEK_SET) == 0);
    char log[512] = {};
    assert(read(output, log, sizeof(log) - 1) > 0);
    assert(strstr(log, "Reinstalling ca-certificates") && !strstr(log, "fixture-secret") &&
        !strstr(log, "https://"));
    close(output);
    close(dir);
    puts("CA recovery: missing/corrupt files, private backup, symlinks, invalid PEM and directory permissions passed");
}
