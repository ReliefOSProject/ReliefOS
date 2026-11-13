#include "image.h"
#include "../fltk_pixels.h"
#include <png.h>
extern "C" {
#include "../paint/model.h"
}

ImagePreview::~ImagePreview() { free(pixels_); }

bool ImagePreview::load(const clip::Entry &entry)
{
    free(pixels_);
    pixels_ = nullptr;
    const clip::Payload *p = entry.find("image/png");
    if (p) {
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&image, p->bytes.c_str(), p->bytes.size()) &&
            image.width && image.height && uint64_t(image.width) * image.height <= 16 * 1024 * 1024) {
            image.format = PNG_FORMAT_RGB;
            clip::Array<unsigned char> rgb;
            const png_color background{255, 255, 255};
            if (rgb.resize(PNG_IMAGE_SIZE(image)) && png_image_finish_read(&image, &background, rgb.data(), 0, nullptr)) {
                size_t count = size_t(image.width) * image.height;
                pixels_ = static_cast<uint32_t *>(malloc(count * sizeof(uint32_t)));
                if (pixels_) {
                    width_ = image.width;
                    height_ = image.height;
                    for (size_t i = 0; i < count; ++i)
                        pixels_[i] = unsigned(rgb[i * 3]) << 16 | unsigned(rgb[i * 3 + 1]) << 8 | rgb[i * 3 + 2];
                }
            }
        }
        png_image_free(&image);
    }
    if (!pixels_ && (p = entry.find("image/bmp")))
        (void)paint_bmp_decode(reinterpret_cast<const uint8_t *>(p->bytes.c_str()), uint32_t(p->bytes.size()),
            &pixels_, &width_, &height_);
    redraw();
    return pixels_ != nullptr;
}

void ImagePreview::draw()
{
    fl_push_clip(x(), y(), w(), h());
    fl_color(FL_WHITE);
    fl_rectf(x(), y(), w(), h());
    if (pixels_ && width_ && height_) {
        unsigned dw = width_, dh = height_;
        if (dw > unsigned(w() - 16)) { dh = uint64_t(dh) * (w() - 16) / dw; dw = w() - 16; }
        if (dh > unsigned(h() - 16)) { dw = uint64_t(dw) * (h() - 16) / dh; dh = h() - 16; }
        if (dw && dh) reliefos_fltk::draw_pixels(pixels_, width_, height_,
            x() + (w() - int(dw)) / 2, y() + (h() - int(dh)) / 2, dw, dh);
    }
    fl_pop_clip();
}
