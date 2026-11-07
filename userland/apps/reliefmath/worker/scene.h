#ifndef RELIEFMATH_SCENE_H
#define RELIEFMATH_SCENE_H
#include "font.h"
#include "../storage.h"
namespace rm {
struct Draw {
    enum class Kind : uint8_t { Glyph, Rule };
    Kind kind=Kind::Glyph;
    uint32_t codepoint=0;
    Face face=Face::Math;
    double x=0,y=0,size=0,xscale=1,yscale=1,width=0,height=0;
};
struct Box {
    double width=0,ascent=0,descent=0;
    Array<Draw> draws;
    bool append(const Box &,double dx,double dy,Error &);
};
bool glyph_box(FontBook &,uint32_t,double,Face,Box &,Error &);
bool rule_box(double width,double thickness,Box &,Error &);
bool parse_latex(const char *,size_t,double size,bool display,FontBook &,Box &,Error &);
}
#endif
