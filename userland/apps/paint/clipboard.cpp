#include "clipboard.h"
#include <FL/Fl.H>
#include <X11/Xatom.h>
#include <png.h>
extern "C" {
#include "model.h"
}

ImageClipboard::~ImageClipboard()
{
    if (!initialized_) return;
    Fl::remove_timeout(tick, this);
    Fl::remove_fd(selection_.fd());
}

bool ImageClipboard::initialize()
{
    if (!selection_.open()) return false;
    initialized_ = true;
    Fl::add_fd(selection_.fd(), [](int, void *p) { static_cast<ImageClipboard *>(p)->selection_.pump(); }, this);
    Fl::add_timeout(.1, tick, this);
    return true;
}

void ImageClipboard::tick(void *context)
{
    auto *self = static_cast<ImageClipboard *>(context);
    self->selection_.pump();
    Fl::repeat_timeout(.1, tick, context);
}

void ImageClipboard::paste()
{
    if (!initialized_) { if (failed) failed(); return; }
    source_ = selection_.owner();
    if (!selection_.fetch(selection_.atom("TARGETS"), targets, this, source_) && failed) failed();
}

void ImageClipboard::targets(int result, const clip::Received &value, void *context)
{
    auto *self = static_cast<ImageClipboard *>(context);
    Atom png = self->selection_.atom("image/png"), bmp = self->selection_.atom("image/bmp"), chosen = None;
    if (!result && value.type == XA_ATOM && value.format == 32) {
        for (size_t i = 0; i < value.bytes.size() / 4; ++i) {
            Atom target = clip::Selection::word(value.bytes, i);
            if (target == png || (target == bmp && chosen == None)) chosen = target;
        }
    }
    if ((!chosen || !self->selection_.fetch(chosen, image, self, self->source_)) && self->failed) self->failed();
}

void ImageClipboard::image(int result, const clip::Received &value, void *context)
{
    auto *self = static_cast<ImageClipboard *>(context);
    uint32_t *pixels = nullptr, width = 0, height = 0;
    if (!result && value.format == 8 && value.type == self->selection_.atom("image/bmp")) {
        (void)paint_bmp_decode(reinterpret_cast<const uint8_t *>(value.bytes.c_str()), uint32_t(value.bytes.size()),
            &pixels, &width, &height);
    } else if (!result && value.format == 8 && value.type == self->selection_.atom("image/png")) {
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&image, value.bytes.c_str(), value.bytes.size()) &&
            image.width && image.height && uint64_t(image.width) * image.height <= PAINT_MAX_PIXELS) {
            image.format = PNG_FORMAT_RGBA;
            clip::Array<unsigned char> rgba;
            if (rgba.resize(PNG_IMAGE_SIZE(image)) && png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) {
                width = image.width; height = image.height;
                pixels = static_cast<uint32_t *>(malloc(size_t(width) * height * sizeof(uint32_t)));
                if (pixels) for (size_t i = 0; i < size_t(width) * height; ++i) {
                    const auto *p = rgba.data() + i * 4;
                    unsigned r = (p[0] * unsigned(p[3]) + 255 * (255 - p[3])) / 255;
                    unsigned g = (p[1] * unsigned(p[3]) + 255 * (255 - p[3])) / 255;
                    unsigned b = (p[2] * unsigned(p[3]) + 255 * (255 - p[3])) / 255;
                    pixels[i] = r << 16 | g << 8 | b;
                }
            }
        }
        png_image_free(&image);
    }
    if (pixels && self->accepted) self->accepted(pixels, width, height);
    else { free(pixels); if (self->failed) self->failed(); }
}
