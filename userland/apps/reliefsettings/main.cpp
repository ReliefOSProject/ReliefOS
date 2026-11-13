#include "window.h"
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--session-environment"))
        return rs::session_environment();
    if (argc == 2 && !strcmp(argv[1], "--apply-session"))
        return rs::apply_session();
    if (argc == 4 && !strcmp(argv[1], "--authenticate-timezone")) {
        if (!rs::valid_zone(argv[2]) || !rs::token(argv[3], "_-.@"))
            return 2;
        // Command parsers use the C locale. Authentication is interactive and
        // must carry the GUI's message language to the unprivileged askpass.
        if (setenv("LC_ALL", argv[3], 1) || setenv("LANG", argv[3], 1) ||
            setenv("SUDO_ASKPASS", "/usr/lib/reliefos/apps/reliefaskpass/reliefaskpass.elf", 1))
            return 1;
        const char *command[] = {"/usr/bin/sudo",  "-A",    "--",   rs::Program,
                                 "--set-timezone", argv[2], nullptr};
        execv(command[0], const_cast<char *const *>(command));
        fprintf(stderr, "[reliefsettings] sudo: %s\n", strerror(errno));
        return 127;
    }
    if (argc == 3 && !strcmp(argv[1], "--set-timezone")) {
        if (geteuid() != 0) {
            fputs("Administrator permission is required.\n", stderr);
            return 1;
        }
        rs::Text error;
        bool published = false;
        if (rs::set_timezone(argv[2], error, "/etc", "/usr/share/zoneinfo", &published))
            return 0;
        fprintf(stderr, "[reliefsettings] %s\n", error.c_str());
        return published ? 3 : 1;
    }
    if (argc != 1) {
        fputs("Usage: reliefsettings\n", stderr);
        return 2;
    }
    reliefos_fltk::init("reliefsettings");
    rs::App app;
    app.show();
    return Fl::run();
}
