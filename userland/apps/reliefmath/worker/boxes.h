#ifndef RELIEFMATH_BOXES_H
#define RELIEFMATH_BOXES_H

#include "scene.h"

namespace rm {

enum class GridLayout : uint8_t { Centered, Cases, Aligned, Gathered };

bool box_make_glyph(FontBook &, uint32_t, double, Face, Box &, Error &);
bool box_make_spacer(double, Box &, Error &);
bool box_place(Box &, const Box &, double, double, Error &);
bool box_scale(Box &, double, Error &);
bool box_fraction(const Box &, const Box &, double, Box &, Error &);
bool box_scripts(const Box &, const Box *, const Box *, double, bool, Box &, Error &);
bool box_radical(FontBook &, const Box &, const Box *, double, Box &, Error &);
bool box_delimiter(FontBook &, uint32_t, double, double, double, Box &, Error &);
bool box_accent(FontBook &, const Box &, uint32_t, bool, bool, double, Box &, Error &);
bool box_grid(const Box *, size_t, size_t, GridLayout, double, Box &, Error &);

} // namespace rm

#endif
