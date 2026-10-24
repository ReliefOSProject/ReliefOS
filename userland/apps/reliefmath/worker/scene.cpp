#include "scene.h"
#include <math.h>
namespace rm {
bool Box::append(const Box &other,double dx,double dy,Error &e) {
    if(!isfinite(dx)||!isfinite(dy)) return e.set(Code::Limit,"Invalid layout coordinates"),false;
    if(!draws.reserve(draws.size()+other.draws.size())) return e.set(Code::NoMemory,"Cannot allocate formula layout"),false;
    for(size_t i=0;i<other.draws.size();i++) { Draw d=other.draws[i];d.x+=dx;d.y+=dy;if(!draws.push(d))return false; }
    return true;
}
bool glyph_box(FontBook &fonts,uint32_t cp,double size,Face face,Box &out,Error &e) {
    GlyphMetrics m;
    if(!fonts.measure(cp,size,face,m,e))return false;
    Box b;b.width=m.advance;b.ascent=m.ascent;b.descent=m.descent;
    Draw d;d.codepoint=cp;d.size=size;d.face=face;
    if(!b.draws.push(d))return e.set(Code::NoMemory,"Cannot allocate glyph layout"),false;
    out=static_cast<Box&&>(b);return true;
}
bool rule_box(double width,double thickness,Box &out,Error &e) {
    Box b;b.width=width;b.ascent=thickness;
    Draw d;d.kind=Draw::Kind::Rule;d.width=width;d.height=thickness;d.y=-thickness;
    if(!b.draws.push(d))return e.set(Code::NoMemory,"Cannot allocate rule"),false;
    out=static_cast<Box&&>(b);return true;
}
}
