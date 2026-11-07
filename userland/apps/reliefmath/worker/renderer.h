#ifndef RELIEFMATH_RENDERER_H
#define RELIEFMATH_RENDERER_H
#include "../render_types.h"
#include "scene.h"
namespace rm {
class Renderer {
    FontBook fonts_;
public:
    bool initialize(const char *resource_root,Error &);
    bool render(const Snapshot &,Rendered &,Error &);
};
bool output_scene(FontBook &,const Box &,const Snapshot &,Rendered &,Error &);
}
#endif
