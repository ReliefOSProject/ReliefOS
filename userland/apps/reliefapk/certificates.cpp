#include "certificates.h"
#include "model.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mbedtls/x509_crt.h>

namespace ra {
namespace {
bool trusted_path(const char *input, bool directory = false) {
    /* This recovery trust anchor is distributed with the OS, independently of
     * the replaceable bundle under /etc. Never accept a user-writable path or
     * a symlink as the source of a privileged trust update. */
    char path[256];
    const size_t length = strlen(input);
    if (length >= sizeof(path)) return false;
    memcpy(path, input, length + 1);
    for (size_t i = 1; i <= length; ++i) {
        if (path[i] && path[i] != '/') continue;
        const char saved = path[i];
        path[i] = 0;
        struct stat info;
        const bool last = i == length;
        const bool valid = lstat(path, &info) == 0 && info.st_uid == 0 &&
            !(info.st_mode & 0022) && (last && !directory ? S_ISREG(info.st_mode) && info.st_size > 0 &&
                info.st_size <= 4 * 1024 * 1024 : S_ISDIR(info.st_mode));
        path[i] = saved;
        if (!valid) return false;
    }
    return true;
}

bool system_ca_path_safe(Text &error) {
    if (!trusted_path("/etc/ssl/certs", true)) {
        error.set("CA parent directories must be root-owned and not group/other writable or symlinks");
        return false;
    }
    const int directory = open("/etc/ssl/certs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const bool safe = directory >= 0 && safe_ca_bundle_at(directory, 0, error);
    if (directory >= 0) close(directory);
    return safe;
}

int run_apk(const char *const *arguments, bool quiet = false) {
    int output[2] = {-1, -1};
    if (!quiet && pipe(output) != 0) return -1;
    if (!quiet) {
        fcntl(output[0], F_SETFD, FD_CLOEXEC);
        fcntl(output[1], F_SETFD, FD_CLOEXEC);
    }
    const pid_t pid = fork();
    if (pid < 0) {
        if (!quiet) { close(output[0]); close(output[1]); }
        return -1;
    }
    if (!pid) {
        if (quiet) {
            const int null = open("/dev/null", O_WRONLY);
            if (null < 0 || dup2(null, STDOUT_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0) _exit(127);
            if (null > STDERR_FILENO) close(null);
        } else {
            close(output[0]);
            if (dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0) _exit(127);
            if (output[1] > STDERR_FILENO) close(output[1]);
        }
        /* This privileged, fixed operation must not inherit APK_CONFIG,
         * libfetch/loader controls, credentials or caller proxy settings.
         * Ignore global config too: it can disable signature/TLS checks.
         * Repository files and trusted keys still use the system defaults. */
        const char *environment[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C", "LC_ALL=C",
            "TERM=dumb", "APK_CONFIG=/dev/null", nullptr};
        execve("/sbin/apk", const_cast<char *const *>(arguments),
            const_cast<char *const *>(environment));
        _exit(127);
    }
    bool complete = true;
    if (!quiet) {
        close(output[1]);
        complete = relay_certificate_apk_output(output[0]);
        close(output[0]);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) ? WEXITSTATUS(status) : complete ? 0 : 1;
}

int installed_status() {
    const char *arguments[] = {"/sbin/apk", "info", "--installed", "--",
        CertificateSourcesPackage, CertificatePackage, nullptr};
    const int code = run_apk(arguments, true);
    // apk info returns the number of missing fixed targets, up to two here.
    return code == 0 ? 1 : (code == 1 || code == 2) ? 0 : -1;
}

bool read_bundle(const char *path, Text &output, bool nofollow = false) {
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (nofollow ? O_NOFOLLOW : 0));
    if (fd < 0) return false;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > 4 * 1024 * 1024) { close(fd); return false; }
    const size_t size = static_cast<size_t>(info.st_size);
    unsigned char *pem = static_cast<unsigned char *>(malloc(size + 1));
    if (!pem) { close(fd); return false; }
    size_t used = 0;
    while (used < size) {
        const ssize_t count = read(fd, pem + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += static_cast<size_t>(count);
    }
    close(fd);
    pem[size] = 0;
    const bool complete = used == size && output.set(reinterpret_cast<char *>(pem), size);
    free(pem);
    return complete;
}

bool usable_bundle(const Text &pem) {
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    const int result = mbedtls_x509_crt_parse(&chain,
        reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1);
    const bool valid = result >= 0 && chain.raw.p && chain.version > 0;
    mbedtls_x509_crt_free(&chain);
    return valid;
}

int run_certificate_apk(bool reinstall, bool simulate) {
    Command command;
    Text error;
    if (!certificate_apk_command(reinstall, simulate, command, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const char *arguments[24];
    if (command.args.size() >= sizeof(arguments) / sizeof(arguments[0])) return 1;
    for (size_t i = 0; i < command.args.size(); ++i) arguments[i] = command.args[i].c_str();
    arguments[command.args.size()] = nullptr;
    return run_apk(arguments);
}
}

int certificate_command(bool simulate) {
    if (!simulate && geteuid() != 0) {
        fputs("Root authentication required for certificate repair\n", stderr);
        return 126;
    }
    if (chdir("/") != 0) return 1;
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("LC_ALL", "C", 1);
    setenv("LANG", "C", 1);
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("SSL_CERT_FILE");
    unsetenv("SSL_CERT_DIR");
    unsetenv("OPENSSL_CONF");
    unsetenv("OPENSSL_MODULES");
    unsetenv("OPENSSL_ENGINES");
    unsetenv("SSLKEYLOGFILE");
    unsetenv("SSL_NO_VERIFY_HOSTNAME");
    unsetenv("SSL_CLIENT_CERT_FILE");
    unsetenv("SSL_CLIENT_KEY_FILE");
    Text trust_error;
    if (!system_ca_path_safe(trust_error)) {
        fprintf(stderr, "Unsafe system CA trust path; no online operation started: %s\n", trust_error.c_str());
        return 1;
    }
    Text system_bundle;
    const bool recovery = !read_bundle("/etc/ssl/certs/ca-certificates.crt", system_bundle, true) ||
        !usable_bundle(system_bundle);
    if (recovery) {
        Text factory;
        if (!trusted_path(BootstrapCaBundle) || !trusted_path("/etc/ssl/certs", true) ||
            !read_bundle(BootstrapCaBundle, factory, true) || !usable_bundle(factory)) {
            fputs("A trusted OS recovery CA bundle is unavailable. Update the ReliefOS trust package or image.\n", stderr);
            return 1;
        }
        if (simulate) {
            fputs("CA RECOVERY PLAN: the system CA file is unusable. Apply will first restore the trusted OS CA bundle, preserving the old regular file in a private backup.\n"
                  "The two CA world requests will be refreshed as plain package names (without version or tag constraints); all other world requests are preserved.\n"
                  "Online package simulation is deferred until trust is restored. The authenticated worker will then check signed repositories, simulate and repair ca-certificates and ca-certificates-bundle together.\n", stderr);
            return 0;
        }
        const int directory = open("/etc/ssl/certs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        Text backup, error;
        const bool restored = directory >= 0 && restore_ca_bundle_at(directory, factory, backup, error);
        if (directory >= 0) close(directory);
        if (!restored) {
            fprintf(stderr, "OS CA recovery failed: %s\n", error.c_str());
            return 1;
        }
        fprintf(stderr, "OS CA baseline restored atomically; previous file backup: %s\n",
            backup.empty() ? "none (file was missing)" : backup.c_str());
    }
    const int installed = installed_status();
    if (installed < 0) {
        fputs("Could not determine whether the system CA package is installed\n", stderr);
        return 1;
    }
    struct stat pending_before;
    const bool had_pending = lstat("/etc/ssl/certs/ca-certificates.crt.apk-new", &pending_before) == 0;
    fputs("Repair targets: ca-certificates and ca-certificates-bundle (package signatures and TLS verification required).\n"
          "Refresh the two world requests as plain package names without version/tag constraints, then reinstall both so source certificates and generated output agree. Other world requests are preserved.\n", stderr);
    /* apk can return success for a simulation that skips an unavailable APK.
     * A URL-only fetch requires an authenticated available package first. Its
     * output is suppressed because configured repository URLs may carry auth. */
    const char *availability[] = {"/sbin/apk", "--no-interactive", "--no-progress", "--timeout", "30",
        "--no-cache", "--check-certificate", "fetch", "--url", "--simulate", "--",
        CertificateSourcesPackage, CertificatePackage, nullptr};
    if (run_apk(availability, true) != 0) {
        fputs("Verified CA package metadata is unavailable. Check HTTPS, repository signatures, keys and sources. The restored OS CA bundle, if any, has been retained.\n", stderr);
        return 1;
    }
    if (run_certificate_apk(false, true) != 0) return 1;
    if (simulate) return installed ? run_certificate_apk(true, true) : 0;
    // add replaces only the two fixed world constraints, including legacy
    // exact pins; fix then restores missing/corrupt source and bundle files.
    // Alpine's trigger must run with the current source set, or it can undo a
    // bundle-only upgrade. Re-simulate before each installed-package change.
    if (run_certificate_apk(false, false) != 0 || run_certificate_apk(true, true) != 0 ||
        run_certificate_apk(true, false) != 0) return 1;
    struct stat pending_after;
    if (lstat("/etc/ssl/certs/ca-certificates.crt.apk-new", &pending_after) == 0 &&
        (!had_pending || pending_before.st_ino != pending_after.st_ino ||
            pending_before.st_mtim.tv_sec != pending_after.st_mtim.tv_sec ||
            pending_before.st_mtim.tv_nsec != pending_after.st_mtim.tv_nsec)) {
        fputs("APK left an updated CA bundle as .apk-new. Check the protected-path policy before treating this as an updated trust store.\n", stderr);
        return 1;
    }
    Text final_bundle;
    if (!system_ca_path_safe(trust_error) ||
        !read_bundle("/etc/ssl/certs/ca-certificates.crt", final_bundle, true) || !usable_bundle(final_bundle)) {
        fputs("The installed CA bundle is still unusable. Check the APK protected-path policy and .apk-new files; update the ReliefOS trust package/configuration.\n", stderr);
        return 1;
    }
    fputs("System CA bundle installed and parsed successfully\n", stderr);
    return 0;
}
}
