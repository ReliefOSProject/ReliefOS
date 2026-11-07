#include "window.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {
bool worker_executable_path(const char *argv0, char *path, size_t capacity) {
    const ssize_t length = readlink("/proc/self/exe", path, capacity - 1);
    if (length > 0 && static_cast<size_t>(length) < capacity - 1) {
        path[length] = 0;
        return true;
    }
    if (!argv0 || !*argv0 || strlen(argv0) >= capacity) return false;
    memcpy(path, argv0, strlen(argv0) + 1);
    return true;
}
}

int main(int argc, char **argv) {
    if (rmail::worker_mode_requested(argc, const_cast<const char *const *>(argv)))
        return rmail::worker_entry_main();

    reliefos_fltk::init("reliefmail");
    setlocale(LC_NUMERIC, "C");
    const char *families[] = {"Liberation Sans", "SimSun", "Liberation Mono"};
    const char *prefixes[] = {"", "B", "I", "P"};
    static char font_names[12][96];
    for (int family = 0; family < 3; ++family) {
        for (int face = 0; face < 4; ++face) {
            const int index = family * 4 + face;
            snprintf(font_names[index], sizeof(font_names[index]), "%s%s", prefixes[face], families[family]);
            Fl::set_font(16 + index, font_names[index]);
        }
    }
    char executable[4096];
    if (!worker_executable_path(argc ? argv[0] : nullptr, executable, sizeof(executable))) {
        fl_alert("%s", T("Could not locate the ReliefMail worker executable."));
        return 1;
    }
    rmail::MailWindow window(executable);
    reliefos_fltk::style(&window);
    if (!window.start()) {
        fl_alert("%s", T("Could not start the local mail profile."));
        return 1;
    }
    window.show();
    puts("[reliefmail.elf] FLTK mail window ready");
    fflush(stdout);
    return Fl::run();
}
