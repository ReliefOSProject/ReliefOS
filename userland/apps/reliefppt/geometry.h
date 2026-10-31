#ifndef RELIEFPPT_GEOMETRY_H
#define RELIEFPPT_GEOMETRY_H
#include "model.h"

namespace rp {
struct Bounds { bool valid = false; Rect rect; };
Bounds selection_bounds(const Deck &, size_t slide, const Array<uint32_t> &ids);
uint32_t hit_test(const Slide &, float x, float y, float tolerance = 4);
bool resize_rect(Rect original, int handle, float dx, float dy, bool keep_aspect,
    float page_w, float page_h, Rect &out);
using Measure = float (*)(void *, const char *, size_t, const Style &);
struct TextLine { size_t start = 0, length = 0; float width = 0; bool paragraph_start = false; };
bool wrap_text(const Object &, Measure, void *, Array<TextLine> &, Text &error);
bool wrap_text(const Object &, Measure, void *, Array<TextLine> &, Text &error,
    bool clip_overflow);
}
#endif
