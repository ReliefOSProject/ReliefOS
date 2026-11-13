#ifndef RELIEFCLIPBOARD_IMAGE_H
#define RELIEFCLIPBOARD_IMAGE_H
#include "../clipboard/data.h"
#include <FL/Fl_Widget.H>

class ImagePreview : public Fl_Widget {
    uint32_t *pixels_ = nullptr;
    unsigned width_ = 0, height_ = 0;
public:
    ImagePreview(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
    ~ImagePreview() override;
    bool load(const clip::Entry &);
    void draw() override;
};
#endif
