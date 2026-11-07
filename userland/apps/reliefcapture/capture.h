#ifndef RELIEFCAPTURE_CAPTURE_H
#define RELIEFCAPTURE_CAPTURE_H
#include "model.h"
namespace rc {
struct WindowInfo { uint64_t id = 0; Rect bounds; char title[128]{}; };
struct Snapshot { Image image; Array<WindowInfo> windows; };
bool capture_screen(Snapshot &, Text &);
}
#endif
