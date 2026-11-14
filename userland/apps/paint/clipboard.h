#ifndef RELIEFOS_PAINT_CLIPBOARD_H
#define RELIEFOS_PAINT_CLIPBOARD_H
#include "../clipboard/selection.h"

class ImageClipboard {
    clip::Selection selection_;
    Window source_ = None;
    bool initialized_ = false;
    static void tick(void *);
    static void targets(int, const clip::Received &, void *);
    static void image(int, const clip::Received &, void *);
public:
    ~ImageClipboard();
    bool initialize();
    void paste();
    void (*accepted)(uint32_t *pixels, unsigned width, unsigned height) = nullptr;
    void (*failed)() = nullptr;
};
#endif
