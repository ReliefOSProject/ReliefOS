#ifndef RELIEFCAPTURE_RENDER_H
#define RELIEFCAPTURE_RENDER_H
#include "model.h"
namespace rc {
void draw_mark(const Mark &, Rect view, int iw, int ih);
void draw_document(const Document &, Rect view, const Mark *pending = nullptr);
bool render_document(const Document &, Image &, Text &);
}
#endif
