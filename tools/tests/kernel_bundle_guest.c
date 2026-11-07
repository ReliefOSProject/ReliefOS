/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <reliefos/kernel_bundle.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* This coordinator tests the OS package/boot transaction, not kernel internals.
 * It runs only as PID 1 of a deliberately marked, private integration image. */

/** @brief Read one bounded regular file without following a final symlink.
 * @param path Borrowed absolute test path.
 * @param bytes Caller-owned storage of capacity bytes.
 * @param capacity Maximum accepted size; a larger input fails.
 * @return Exact byte count or -1 with errno set.
 */
static ssize_t read_file(const char *path, void *bytes, size_t capacity)
{
    int file = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) return -1;
    struct stat status;
    ssize_t result = -1;
    if (!fstat(file, &status) && S_ISREG(status.st_mode) && status.st_size >= 0 &&
        (size_t)status.st_size <= capacity) {
        size_t used = 0;
        while (used < (size_t)status.st_size) {
            ssize_t count = read(file, (char *)bytes + used, (size_t)status.st_size - used);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            used += (size_t)count;
        }
        if (used == (size_t)status.st_size) result = (ssize_t)used;
    }
    int error = errno;
    close(file);
    errno = error;
    return result;
}

/** @brief Keep a failed, already admitted guest available for serial/QMP diagnosis.
 * @param value Required true invariant.
 * @param operation Borrowed diagnostic description.
 */
static void require(int value, const char *operation)
{
    if (value) return;
    fprintf(stderr, "FAIL KERNEL_BUNDLE: %s errno=%d (%s)\n", operation, errno, strerror(errno));
    for (;;) pause();
}

/** @brief Execute the real target CLI and require its normal successful exit.
 * @param release Borrowed expected installed release.
 * @param check Nonzero selects read-only package validation.
 */
static void run_publisher(const char *release, int check)
{
    pid_t child = fork();
    require(child >= 0, "fork real kernel publisher");
    if (!child) {
        if (check) execl("/usr/sbin/reliefos-boot-install", "reliefos-boot-install",
            "--kernel-release", release, "--root-directory", "/", "--boot-directory", "/boot", "--check", (char *)NULL);
        else execl("/usr/sbin/reliefos-boot-install", "reliefos-boot-install",
            "--kernel-release", release, "--root-directory", "/", "--boot-directory", "/boot", (char *)NULL);
        _exit(127);
    }
    int status; pid_t waited;
    do waited = waitpid(child, &status, 0); while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && !WEXITSTATUS(status), "actual installed bundle CLI");
}

/** @brief Persist a bounded QA phase only after the publication being observed.
 * @param phase Borrowed canonical phase string.
 */
static void store_phase(const char *phase)
{
    int file = open("/ko-bundle-phase", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(file >= 0, "create owned phase marker");
    size_t size = strlen(phase), used = 0;
    while (used < size) {
        ssize_t count = write(file, phase + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "write phase marker");
        used += (size_t)count;
    }
    require(!fsync(file) && !close(file), "flush phase contents");
    file = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(file >= 0 && !fsync(file) && !close(file), "flush phase directory");
}

/** @brief Simulate APK's removal of only the admitted old package's owned files.
 * @param release Borrowed already validated old release from this owned image.
 * The package has already been validated and durably retained by the real
 * static tool. Each removed directory entry is synced before the deliberate
 * reboot, so recovery cannot accidentally pass using uncommitted unlinks.
 */
static void remove_old_package(const char *release)
{
    pid_t child = fork(); require(child >= 0, "fork actual static retainer");
    if (!child) {
        execl("/usr/libexec/reliefos/kernel-bundle", "kernel-bundle", "--retain", "/", release, (char *)NULL);
        _exit(127);
    }
    int status; require(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "real retained copy completed");
    char sums[128], modules[96], prefix[96];
    snprintf(prefix, sizeof(prefix), "usr/lib/reliefos/kernel/%s/", release);
    snprintf(modules, sizeof(modules), "lib/modules/%s/", release);
    snprintf(sums, sizeof(sums), "/%sSHA256SUMS", prefix);
    FILE *stream = fopen(sums, "r"); require(stream != NULL, "open checked old inventory");
    char *line = NULL; size_t capacity = 0; ssize_t size;
    while ((size = getline(&line, &capacity, stream)) >= 0) {
        require(size >= 68 && line[size - 1] == '\n' && line[64] == ' ' && line[65] == ' ', "bounded old inventory record");
        line[size - 1] = 0;
        const char *name = line + 66;
        require((!strncmp(name, prefix, strlen(prefix)) || !strncmp(name, modules, strlen(modules))) && !strstr(name, ".."),
                "removal confined to checked old release");
        char path[1024]; int length = snprintf(path, sizeof(path), "/%s", name);
        require(length > 0 && (size_t)length < sizeof(path) && !unlink(path), "remove one old package member");
        char *slash = strrchr(path, '/'); require(slash != NULL, "owned member parent"); *slash = 0;
        int directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(directory >= 0 && !fsync(directory) && !close(directory), "flush removed old member directory");
    }
    require(!ferror(stream) && !fclose(stream), "complete old removal inventory"); free(line);
    require(!unlink(sums), "remove old package checksum list last");
    *strrchr(sums, '/') = 0;
    int directory = open(sums, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(directory >= 0 && !fsync(directory) && !close(directory), "flush removed checksum list directory");
}

/** @brief Exercise real signed APK upgrade hooks only on an explicit owned image.
 * The fixture has a real installed A database, a signed B repository and its
 * private trusted QA public key. No network or trust bypass is permitted.
 */
static void maybe_upgrade_apk(void)
{
    static const char expected[] = "ReliefOS owned signed APK kernel upgrade QA\n";
    char marker[sizeof(expected)];
    ssize_t size = read_file("/ko-bundle-apk-upgrade", marker, sizeof(marker));
    if (size < 0 && errno == ENOENT) return;
    require(size == sizeof(expected) - 1 && !memcmp(marker, expected, sizeof(expected) - 1), "explicit signed APK upgrade opt-in");
    char phase[64];
    size = read_file("/ko-bundle-phase", phase, sizeof(phase));
    if (size >= 0) return;
    require(errno == ENOENT, "initial signed APK phase");
    static const char rpr_expected[] = "ReliefOS owned signed RPR client upgrade QA\n";
    char rpr_marker[sizeof(rpr_expected)];
    ssize_t rpr_size = read_file("/ko-bundle-rpr-upgrade", rpr_marker, sizeof(rpr_marker));
    int rpr = rpr_size >= 0;
    require(rpr ? rpr_size == sizeof(rpr_expected) - 1 && !memcmp(rpr_marker, rpr_expected, sizeof(rpr_expected) - 1)
                : errno == ENOENT, "explicit RPR client upgrade opt-in");
    pid_t child = fork(); require(child >= 0, "fork real upstream APK");
    if (!child) {
        if (rpr) execl("/usr/sbin/reliefos-kernel-update", "reliefos-kernel-update", (char *)NULL);
        else execl("/sbin/apk", "apk", "--repositories-file", "/qa-repositories", "--no-network",
              "upgrade", "reliefos-kernel", (char *)NULL);
        _exit(127);
    }
    int status; require(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
                        "real signed APK upgrade, retention hooks and package activation");
    FILE *world = fopen("/etc/apk/world", "r"); require(world != NULL, "read actual APK world");
    char *line = NULL; size_t capacity = 0; unsigned found = 0;
    while (getline(&line, &capacity, world) >= 0) {
        if (!strncmp(line, "reliefos-kernel", strlen("reliefos-kernel"))) {
            require(!strcmp(line, "reliefos-kernel\n"), "APK world keeps a plain kernel name without an exact version pin");
            ++found;
        }
    }
    require(!ferror(world) && !fclose(world) && found == 1, "one plain kernel world request"); free(line);
    puts("PASS KERNEL_BUNDLE_SIGNED_APK real signed offline APK upgrade, native retention and activation, plain world");
    if (rpr) puts("PASS KERNEL_BUNDLE_RPR_CLIENT actual frontend, signed frozen cohort, complete bundle preflight and native activation");
}

/** @brief Prove publication, a different real kernel boot, and firmware fallback.
 * @return 77 on every host/unowned invocation before any mutation; admitted
 * guests pause with a stable PASS/FAIL token for the private image runner.
 */
int main(void)
{
    struct utsname system;
    if (uname(&system) || strcmp(system.sysname, "ReliefNT") || getpid() != 1) {
        fputs("Guest-only bundle probe: refusing host mutations\n", stderr);
        return 77;
    }
    char marker[128];
    static const char expected[] = "ReliefOS owned kernel bundle integration QA\n";
    if (read_file("/ko-bundle-owned", marker, sizeof(marker)) != sizeof(expected) - 1 ||
        memcmp(marker, expected, sizeof(expected) - 1)) {
        fputs("Guest-only bundle probe: refusing unowned root\n", stderr);
        return 77;
    }
    int serial = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
    if (serial >= 0) { dup2(serial, 1); dup2(serial, 2); if (serial > 2) close(serial); }
    setvbuf(stdout, NULL, _IONBF, 0);
    char releases[96] = {0}, old[32], next[32], tail;
    ssize_t size = read_file("/ko-bundle-releases", releases, sizeof(releases) - 1);
    require(size > 0 && sscanf(releases, "%31s\n%31s %c", old, next, &tail) == 2 && strcmp(old, next),
            "two different expected real kernel releases");
    struct statfs fs;
    require(!statfs("/", &fs) && fs.f_type == 0xef53, "production /init reached actual ext root");
    require(!statfs("/boot", &fs) && fs.f_type == 0x4d44, "production /init mounted real fstab ESP");
    maybe_upgrade_apk();
    run_publisher(old, 1);
    run_publisher(next, 1);
    unsigned char a[RELIEFOS_KERNEL_SLOT_SIZE], before[sizeof(a)], b[sizeof(a)];
    require(read_file("/boot/reliefos/state/kernel-A.env", a, sizeof(a)) == sizeof(a), "read retained A record");
    struct reliefos_kernel_slot slot;
    require(!reliefos_kernel_slot_decode(a, sizeof(a), &slot) && slot.generation == 1 && !strcmp(slot.release, old),
            "old A record is still complete");
    char phase[64] = {0};
    size = read_file("/ko-bundle-phase", phase, sizeof(phase) - 1);
    int first = size < 0 && errno == ENOENT;
    if (first) {
        static const char cut[] = "ReliefOS owned interrupted APK removal QA\n";
        char marker[sizeof(cut)]; ssize_t count = read_file("/ko-bundle-retention-cut", marker, sizeof(marker));
        if (count >= 0) {
            require(count == sizeof(cut) - 1 && !memcmp(marker, cut, sizeof(cut) - 1), "explicit owned removal opt-in");
            require(!strcmp(system.release, old), "retention cut starts under actual old kernel");
            remove_old_package(old);
            store_phase("A removed\n");
            printf("PASS KERNEL_BUNDLE_RETAINED_REMOVAL actual=%s old-owned-files-unlinked-and-synced\n", old);
            for (;;) pause();
        }
        require(errno == ENOENT, "optional removal opt-in is absent");
    }
    if (first || !strcmp(phase, "A removed\n")) {
        require(!strcmp(system.release, old), "publication starts under the old kernel");
        memcpy(before, a, sizeof(a));
        run_publisher(next, 0);
        require(read_file("/boot/reliefos/state/kernel-A.env", a, sizeof(a)) == sizeof(a) && !memcmp(a, before, sizeof(a)),
                "publication left current slot byte-identical");
        require(read_file("/boot/reliefos/state/kernel-B.env", b, sizeof(b)) == sizeof(b) &&
                !reliefos_kernel_slot_decode(b, sizeof(b), &slot) && slot.generation == 2 && !strcmp(slot.release, next),
                "inactive B record names the different complete kernel");
        store_phase("B published\n");
        printf("PASS KERNEL_BUNDLE_PUBLISHED old=%s next=%s generation=2\n", old, next);
    } else if (!strcmp(phase, "B published\n")) {
        require(!strcmp(system.release, next), "actual second boot runs the published different kernel");
        run_publisher(next, 0); /* The same complete selection must be idempotent. */
        store_phase("B booted\n");
        printf("PASS KERNEL_BUNDLE_REBOOT actual=%s old-root-package-retained=%s\n", system.release, old);
    } else {
        require(!strcmp(phase, "B booted\n") && !strcmp(system.release, old),
                "corrupt new boot pair falls back to actual old kernel");
        printf("PASS KERNEL_BUNDLE_ROLLBACK actual=%s both-root-packages-verified\n", system.release);
    }
    for (;;) pause();
}
