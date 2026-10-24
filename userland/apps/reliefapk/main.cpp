#include "window.h"
#include "certificates.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
namespace {
int save_config(int argc, char **argv) {
    if (geteuid() != 0 || argc != 4 || !ra::standard_repository_path(argv[1])) {
        fputs("Invalid privileged configuration request\n", stderr);
        return 126;
    }
    ra::Text error;
    if (!ra::save_repository_file(argv[1], argv[2], argv[3], error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    puts("Repositories saved; the previous file is available as .reliefapk.bak");
    return 0;
}
bool valid_apk_worker(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[0], "/sbin/apk"))
        return false;
    const char *commands[] = {"add", "del", "fix", "upgrade", "update", "cache", nullptr};
    const char *flags[] = {"--no-network", "--no-interactive", "--no-progress", "--simulate", "--reinstall",
                           "--upgrade",    "--available",      "--purge",       "--latest",   nullptr};
    bool command = false, operands = false, allow_local = false;
    for (int i = 1; i < argc; ++i) {
        const char *s = argv[i];
        if (!s || strlen(s) > 8192)
            return false;
        if (operands) {
            if (!(ra::valid_constraint(s) || (allow_local && ra::valid_constraint(s, true))))
                return false;
            continue;
        }
        if (!strcmp(s, "--")) {
            operands = true;
            continue;
        }
        bool known = false;
        for (const char **p = flags; *p; ++p)
            if (!strcmp(*p, s))
                known = true;
        if (known)
            continue;
        if (!strcmp(s, "--cache-dir")) {
            if (++i >= argc || argv[i][0] != '/')
                return false;
            ra::Text cache_error;
            if (!ra::secure_cache_directory(argv[i], cache_error)) {
                fprintf(stderr, "%s\n", cache_error.c_str());
                return false;
            }
            for (const unsigned char *p = reinterpret_cast<const unsigned char *>(argv[i]); *p; ++p)
                if (*p < 32 || *p == 127)
                    return false;
            continue;
        }
        if (!strcmp(s, "--timeout") || !strcmp(s, "--cache-max-age")) {
            if (++i >= argc || !*argv[i])
                return false;
            for (const char *p = argv[i]; *p; ++p)
                if (*p < '0' || *p > '9')
                    return false;
            continue;
        }
        for (const char **p = commands; *p; ++p)
            if (!strcmp(*p, s)) {
                if (command)
                    return false;
                command = true;
                allow_local = !strcmp(s, "add");
                known = true;
                break;
            }
        if (command && (!strcmp(s, "clean") || !strcmp(s, "download")))
            known = true;
        if (!known)
            return false;
    }
    return command;
}
int worker(int argc, char **argv) {
    if (geteuid() != 0) {
        fputs("Root authentication required\n", stderr);
        return 126;
    }
    if (argc == 3 && !strcmp(argv[0], ra::WorkerPath) && !strcmp(argv[1], "--ca-command") &&
        !strcmp(argv[2], "--apply")) {
        fputs("__RELIEFAPK_WORKER_READY__\n", stderr);
        fflush(stderr);
        return ra::certificate_command(false);
    }
    if (argc == 5 && !strcmp(argv[0], ra::WorkerPath) && !strcmp(argv[1], "--save-repositories") &&
        ra::standard_repository_path(argv[2])) {
        fputs("__RELIEFAPK_WORKER_READY__\n", stderr);
        fflush(stderr);
        return save_config(argc - 1, argv + 1);
    }
    if (!valid_apk_worker(argc, argv)) {
        fputs("Invalid privileged apk request\n", stderr);
        return 126;
    }
    fputs("__RELIEFAPK_WORKER_READY__\n", stderr);
    fflush(stderr);
    chdir("/");
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("LC_ALL", "C", 1);
    setenv("LANG", "C", 1);
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
    execv("/sbin/apk", argv);
    fprintf(stderr, "Cannot execute /sbin/apk: %s\n", strerror(errno));
    return 127;
}
void ready(void *data) {
    auto *app = static_cast<ra::App *>(data);
    app->request_catalog();
    if (!app->initial_path.empty())
        app->queue_target(ra::Action::Install, app->initial_path.c_str());
    if (app->initial_ca_repair) {
        app->settings.offline = false;
        app->queue_target(ra::Action::RepairCa, ra::CertificatePackage);
        app->preview_queue();
    }
    puts("[reliefapk.elf] FLTK package manager ready");
    fflush(stdout);
}
} // namespace
int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--worker"))
        return worker(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "--save-repositories"))
        return save_config(argc - 1, argv + 1);
    if (argc >= 2 && !strcmp(argv[1], "--ca-command")) {
        if (argc == 3 && !strcmp(argv[2], "--simulate")) return ra::certificate_command(true);
        if (argc == 3 && !strcmp(argv[2], "--apply") && geteuid() == 0)
            return ra::certificate_command(false);
        return 126;
    }
    const bool ca_repair = argc == 2 && !strcmp(argv[1], "--repair-ca");
    if (argc > 2 || (argc == 2 && !ca_repair && !(ra::valid_constraint(argv[1]) || ra::valid_constraint(argv[1], true)))) {
        fputs("Usage: reliefapk [package | /absolute/path/package.apk | --repair-ca]\n", stderr);
        return 2;
    }
    reliefos_fltk::init("reliefapk");
    ra::App app(argc == 2 && !ca_repair ? argv[1] : nullptr);
    app.initial_ca_repair = ca_repair;
    app.show();
    Fl::add_timeout(.05, ready, &app);
    return Fl::run();
}
