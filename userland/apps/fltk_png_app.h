#ifndef RELIEFOS_FLTK_PNG_APP_H
#define RELIEFOS_FLTK_PNG_APP_H
#include "fltk_support.h"
#include "fltk_pixels.h"
#include <FL/Fl_Double_Window.H>
#include <stdio.h>
#include <string.h>
extern "C" {
#include <reliefos/fs.h>
#include <reliefos/png.h>
}

namespace reliefos_fltk {
template<class Fit> class PngCanvas : public Fl_Widget {
    uint32_t *pixels_ = nullptr;
    unsigned width_ = 0, height_ = 0;
    void (*fit_)(Fit *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
public:
    PngCanvas(const char *app, const char *argv0,
        void (*fit)(Fit *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
        : Fl_Widget(0, 0, 760, 760), fit_(fit)
    {
        char path[RELIEFOS_FS_PATH_LEN];
        int length = snprintf(path, sizeof(path), RELIEFOS_LAYOUT_RELIEFOS_APPS "/%s/%s.png", app, app);
        if (length > 0 && length < static_cast<int>(sizeof(path)) &&
            reliefos_png_decode_file(path, &pixels_, &width_, &height_) == 0) return;
        const char *slash = argv0 ? strrchr(argv0, '/') : nullptr;
        if (slash) {
            length = snprintf(path, sizeof(path), "%.*s/%s.png", static_cast<int>(slash - argv0), argv0, app);
            if (length > 0 && length < static_cast<int>(sizeof(path)) &&
                reliefos_png_decode_file(path, &pixels_, &width_, &height_) == 0) return;
        }
        length = snprintf(path, sizeof(path), "%s.png", app);
        if (length > 0 && length < static_cast<int>(sizeof(path))) reliefos_png_decode_file(path, &pixels_, &width_, &height_);
    }
    ~PngCanvas() override { reliefos_png_free(pixels_); }
    void draw() override
    {
        fl_push_clip(x(), y(), w(), h());
        fl_color(FL_BLACK); fl_rectf(x(), y(), w(), h());
        if (pixels_ && width_ && height_) {
            Fit fit{}; fit_(&fit, w(), h(), 8, width_, height_);
            draw_pixels(pixels_, width_, height_, x()+fit.x, y()+fit.y, fit.w, fit.h);
        } else {
            fl_font(FL_HELVETICA, 15); fl_color(FL_WHITE);
            fl_draw(T("Could not decode image"), x()+12, y()+12, w()-24, h()-24, FL_ALIGN_CENTER);
        }
        fl_pop_clip();
    }
};
template<class Fit> int png_main(int argc, char **argv, const char *app,
    void (*fit)(Fit *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
{
    init();
    Fl_Double_Window window(760, 760, app);
    PngCanvas<Fit> *canvas = new PngCanvas<Fit>(app, argc ? argv[0] : nullptr, fit);
    window.end(); window.resizable(canvas); window.size_range(120, 120);
    style(&window); window.show();
    printf("[%s.elf] FLTK image ready\n", app);
    return Fl::run();
}
}
#endif
