#ifndef RELIEFPPT_RENDER_H
#define RELIEFPPT_RENDER_H
#include "geometry.h"
#include <FL/Enumerations.H>
namespace rp {
Fl_Font style_font(const Style &);
Fl_Font editing_font(const Style &, const char *, size_t);
float measure_text(void *, const char *, size_t, const Style &);
bool draw_slide(const Deck &, size_t, int x, int y, float scale, const Array<Object> *preview = nullptr,
                Text *error = nullptr);
bool render_png(const Deck &, size_t, Text &png, Text &error);
using ExportProgress = bool (*)(void *, unsigned, unsigned);
bool render_html(const Deck &, Text &html, Text &error, ExportProgress progress = nullptr,
                 void *context = nullptr);
bool load_image(const char *, Asset &, Text &error);
bool text_overflows(const Object &);
} // namespace rp
#endif
