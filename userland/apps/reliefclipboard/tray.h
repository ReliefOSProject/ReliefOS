#ifndef RELIEFCLIPBOARD_TRAY_H
#define RELIEFCLIPBOARD_TRAY_H
#include "../clipboard/selection.h"

class Tray {
    clip::Selection &connection_;
    Window window_ = None;
    Atom tray_ = None;
    void dock();
    void draw();
public:
    explicit Tray(clip::Selection &connection) : connection_(connection) {}
    ~Tray();
    void initialize();
    void event(const XEvent &);
    void (*clicked)(bool menu, int x, int y, void *) = nullptr;
    void *context = nullptr;
};
#endif
