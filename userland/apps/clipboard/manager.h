#ifndef RELIEFOS_CLIPBOARD_MANAGER_H
#define RELIEFOS_CLIPBOARD_MANAGER_H
#include "history.h"
#include "selection.h"

namespace clip {
class Manager {
public:
    Selection selection;
    History history;
    bool paused = false;
    bool stopped = false;
    void (*show)(void *) = nullptr;
    Selection::Events events = nullptr;
    void *context = nullptr;
    bool initialize();
    void changed();
    void pump();
    bool use(size_t index);
    void clear();
    const char *error() const { return error_.c_str(); }
private:
    Window observed_ = None, source_ = None;
    Atom manager_ = None;
    Time acquired_ = CurrentTime;
    Array<Atom> targets_;
    Array<XSelectionRequestEvent> saves_;
    size_t next_target_ = 0;
    uint32_t timestamp_ = 0;
    bool checking_timestamp_ = false;
    Entry capturing_, last_;
    bool valid_ = false, capturing = false;
    Text error_;
    static void event(const XEvent &, void *);
    static void targets(int, const Received &, void *);
    static void payload(int, const Received &, void *);
    static void timestamp(int, const Received &, void *);
    void next();
    void finish();
    void request(const XSelectionRequestEvent &);
    void reply(const XSelectionRequestEvent &, bool success);
};
} // namespace clip
#endif
