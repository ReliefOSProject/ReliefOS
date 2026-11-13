#ifndef RELIEFOS_FLTK_CLIPBOARD_H
#define RELIEFOS_FLTK_CLIPBOARD_H
#include <FL/Fl.H>
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>

struct _XDisplay;
extern _XDisplay *fl_display;
extern unsigned long fl_message_window;

namespace reliefos_fltk {
inline void save_clipboard_at_exit()
{
    using Intern = unsigned long (*)(_XDisplay *, const char *, int);
    using Owner = unsigned long (*)(_XDisplay *, unsigned long);
    using Convert = int (*)(_XDisplay *, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
    using Flush = int (*)(_XDisplay *);
    auto intern = reinterpret_cast<Intern>(dlsym(RTLD_DEFAULT, "XInternAtom"));
    auto owner = reinterpret_cast<Owner>(dlsym(RTLD_DEFAULT, "XGetSelectionOwner"));
    auto convert = reinterpret_cast<Convert>(dlsym(RTLD_DEFAULT, "XConvertSelection"));
    auto flush = reinterpret_cast<Flush>(dlsym(RTLD_DEFAULT, "XFlush"));
    if (!fl_display || !fl_message_window || !intern || !owner || !convert || !flush) return;
    unsigned long clipboard = intern(fl_display, "CLIPBOARD", 0);
    unsigned long manager = intern(fl_display, "CLIPBOARD_MANAGER", 0);
    if (owner(fl_display, clipboard) != fl_message_window || !owner(fl_display, manager)) return;
    unsigned long target = intern(fl_display, "SAVE_TARGETS", 0);
    convert(fl_display, manager, target, target, fl_message_window, 0);
    flush(fl_display);
    struct timespec started{}, current{};
    clock_gettime(CLOCK_MONOTONIC, &started);
    do {
        Fl::wait(.01);
        clock_gettime(CLOCK_MONOTONIC, &current);
    } while (owner(fl_display, clipboard) == fl_message_window && current.tv_sec - started.tv_sec < 10);
}

inline void initialize_clipboard_handoff()
{
    static bool registered = false;
    if (!registered) { atexit(save_clipboard_at_exit); registered = true; }
}
}
#endif
