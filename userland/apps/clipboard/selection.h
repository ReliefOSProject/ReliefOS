#ifndef RELIEFOS_CLIPBOARD_SELECTION_H
#define RELIEFOS_CLIPBOARD_SELECTION_H
#include "data.h"
#include <X11/Xlib.h>

namespace clip {
struct Received {
    Atom type = None;
    int format = 0;
    Text bytes;
};

class Selection {
public:
    using Completion = void (*)(int, const Received &, void *);
    using Events = void (*)(const XEvent &, void *);
    Selection() = default;
    ~Selection();
    bool open();
    Display *display() const { return display_; }
    Window window() const { return window_; }
    Atom atom(const char *name) const;
    int fd() const;
    Time server_time();
    Window owner() const;
    bool publish(const Entry &entry);
    bool owns() const;
    const Entry &entry() const { return owned_; }
    bool fetch(Atom target, Completion, void *, Window expected = None);
    bool receiving() const { return incoming_.window != None; }
    void cancel();
    void pump();
    Events events = nullptr;
    void *context = nullptr;
    static uint32_t word(const Text &, size_t index = 0);
private:
    Display *display_ = nullptr;
    Window window_ = None;
    Atom clipboard_ = None, property_ = None, incr_ = None;
    Time acquired_ = CurrentTime;
    Entry owned_;
    struct Incoming {
        Window window = None, owner = None;
        Atom target = None, type = None;
        int format = 0;
        bool incremental = false;
        double deadline = 0;
        Array<unsigned char> bytes;
        Completion complete = nullptr;
        void *data = nullptr;
    } incoming_;
    struct Outgoing {
        Window window = None;
        Atom property = None, type = None;
        Text bytes;
        size_t offset = 0;
        double deadline = 0;
    };
    Array<Outgoing> outgoing_;
    bool convert(Window requestor, Atom target, Atom property);
    void request(const XSelectionRequestEvent &);
    void property(const XPropertyEvent &);
    void notify(const XSelectionEvent &);
    void complete(int result);
    bool append(const Received &);
    int read_property(Window, Atom, Received &);
};
double now();
} // namespace clip
#endif
