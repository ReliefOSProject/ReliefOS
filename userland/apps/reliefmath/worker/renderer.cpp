#include "renderer.h"
#include "../limits.h"
#include <math.h>
#include <stdio.h>
#include <unistd.h>
namespace rm {
bool Renderer::initialize(const char *root,Error &e) {
    char math[4096],text[4096];
    if(!root||snprintf(math,sizeof math,"%s/STIXTwoMath-Regular.otf",root)>=int(sizeof math)||
        snprintf(text,sizeof text,"%s/simsun.ttc",root)>=int(sizeof text))
        return e.set(Code::Font,"Formula resource path is too long"),false;
    const char *chinese=access(text,R_OK)==0?text:"/usr/share/fonts/reliefos/simsun.ttc";
    return fonts_.open(math,chinese,e);
}
bool Renderer::render(const Snapshot &input,Rendered &out,Error &e) {
    if(input.formula.source.size()>65536 || !isfinite(input.scale) || input.scale<=0 || input.scale>64 ||
       input.formula.style.point_size<kMinPointSize || input.formula.style.point_size>kMaxPointSize ||
       input.dpi<24 || input.dpi>2400 || !(input.artifacts&7) || (input.artifacts&~7))
        return e.set(Code::Limit,"Formula render settings exceed supported limits"),false;
    Box box;
    double size=input.formula.style.point_size*input.dpi/72.0*input.scale;
    if(!parse_latex(input.formula.source.c_str(),input.formula.source.size(),size,input.formula.style.display,fonts_,box,e))return false;
    Rendered result; result.snapshot=input;result.formula_id=input.formula.id;
    result.request_id=input.request_id;result.generation=input.formula.generation;
    if(!output_scene(fonts_,box,input,result,e))return false;
    out=static_cast<Rendered&&>(result); return true;
}
}
