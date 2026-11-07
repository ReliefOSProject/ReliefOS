#include "font.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <cairo.h>
#include <math.h>
namespace rm {
static uint32_t mapped(uint32_t cp,Face face) {
    if(face==Face::Blackboard) {
        const char *exceptions="CHNPQRZ"; const uint32_t codes[]={0x2102,0x210d,0x2115,0x2119,0x211a,0x211d,0x2124};
        for(unsigned i=0;i<7;i++) if(cp==uint32_t(exceptions[i])) return codes[i];
        if(cp>='A'&&cp<='Z') return 0x1d538+cp-'A';
        if(cp>='a'&&cp<='z') return 0x1d552+cp-'a';
        if(cp>='0'&&cp<='9') return 0x1d7d8+cp-'0';
    }
    if(face==Face::Calligraphic) {
        const char *exceptions="BEFHILMR"; const uint32_t codes[]={0x212c,0x2130,0x2131,0x210b,0x2110,0x2112,0x2133,0x211b};
        for(unsigned i=0;i<8;i++) if(cp==uint32_t(exceptions[i])) return codes[i];
        if(cp>='A'&&cp<='Z') return 0x1d49c+cp-'A';
    }
    if(face==Face::Bold) {
        if(cp>='A'&&cp<='Z') return 0x1d400+cp-'A';
        if(cp>='a'&&cp<='z') return 0x1d41a+cp-'a';
        if(cp>='0'&&cp<='9') return 0x1d7ce + cp-'0';
    }
    if(face==Face::Math||face==Face::Italic) {
        if(cp=='h') return 0x210e;
        if(cp>='A'&&cp<='Z') return 0x1d434+cp-'A';
        if(cp>='a'&&cp<='z') return 0x1d44e + cp-'a';
    }
    return cp;
}
FontBook::~FontBook() {
    if(math_) FT_Done_Face(static_cast<FT_Face>(math_));
    if(text_) FT_Done_Face(static_cast<FT_Face>(text_));
    if(library_) FT_Done_FreeType(static_cast<FT_Library>(library_));
}
bool FontBook::open(const char *math_path,const char *text_path,Error &e) {
    if(library_) return e.set(Code::Font,"Font book already initialized"),false;
    FT_Library library=nullptr; FT_Face math=nullptr,text=nullptr;
    if(FT_Init_FreeType(&library)) return e.set(Code::Font,"Cannot initialize FreeType"),false;
    if(FT_New_Face(library,math_path,0,&math)||FT_New_Face(library,text_path,0,&text)) {
        if(math) FT_Done_Face(math);
        if(text) FT_Done_Face(text);
        FT_Done_FreeType(library); return e.set(Code::Font,"Cannot load required formula fonts"),false;
    }
    if(!(math->face_flags&FT_FACE_FLAG_SCALABLE)||!(text->face_flags&FT_FACE_FLAG_SCALABLE)) {
        FT_Done_Face(math); FT_Done_Face(text); FT_Done_FreeType(library);
        return e.set(Code::Font,"Formula fonts must have scalable outlines"),false;
    }
    library_=library; math_=math; text_=text; return true;
}
bool FontBook::select(uint32_t cp,double size,Face face,void *&font,uint32_t &index,Error &e) {
    if(!library_||!isfinite(size)||size<=0||size>16384) return e.set(Code::Font,"Invalid glyph size"),false;
    uint32_t target=mapped(cp,face); FT_Face selected=static_cast<FT_Face>(math_);
    index=FT_Get_Char_Index(selected,target);
    if(!index) { selected=static_cast<FT_Face>(text_); index=FT_Get_Char_Index(selected,target); }
    if(!index) return e.set(Code::Font,"Required glyph is missing from formula fonts"),false;
    if(FT_Set_Char_Size(selected,0,FT_F26Dot6(size*64),72,72)||
       FT_Load_Glyph(selected,index,FT_LOAD_NO_BITMAP|FT_LOAD_NO_HINTING))
        return e.set(Code::Font,"Cannot load glyph outline"),false;
    font=selected; return true;
}
bool FontBook::measure(uint32_t cp,double size,Face face,GlyphMetrics &m,Error &e) {
    void *font=nullptr; uint32_t index=0;
    if(!select(cp,size,face,font,index,e)) return false;
    FT_GlyphSlot glyph=static_cast<FT_Face>(font)->glyph;
    m.advance=glyph->advance.x/64.; m.left=glyph->metrics.horiBearingX/64.;
    m.ascent=glyph->metrics.horiBearingY/64.;
    m.descent=(glyph->metrics.height-glyph->metrics.horiBearingY)/64.;
    if(m.ascent<0)m.ascent=0;
    if(m.descent<0)m.descent=0;
    return true;
}
struct OutlineContext { cairo_t *cr; double x=0,y=0; bool started=false; };
static int move_to(const FT_Vector *p,void *v) {
    OutlineContext &c=*static_cast<OutlineContext *>(v);
    if(c.started)cairo_close_path(c.cr);
    c.x=p->x/64.; c.y=-p->y/64.; cairo_move_to(c.cr,c.x,c.y); c.started=true; return 0;
}
static int line_to(const FT_Vector *p,void *v) {
    OutlineContext &c=*static_cast<OutlineContext *>(v);
    c.x=p->x/64.;c.y=-p->y/64.;cairo_line_to(c.cr,c.x,c.y);return 0;
}
static int conic_to(const FT_Vector *a,const FT_Vector *b,void *v) {
    OutlineContext &c=*static_cast<OutlineContext *>(v);
    double ax=a->x/64.,ay=-a->y/64.,bx=b->x/64.,by=-b->y/64.;
    cairo_curve_to(c.cr,c.x+(ax-c.x)*2/3,c.y+(ay-c.y)*2/3,bx+(ax-bx)*2/3,by+(ay-by)*2/3,bx,by);
    c.x=bx;c.y=by;return 0;
}
static int cubic_to(const FT_Vector *a,const FT_Vector *b,const FT_Vector *d,void *v) {
    OutlineContext &c=*static_cast<OutlineContext *>(v); c.x=d->x/64.;c.y=-d->y/64.;
    cairo_curve_to(c.cr,a->x/64.,-a->y/64.,b->x/64.,-b->y/64.,c.x,c.y);return 0;
}
bool FontBook::outline(uint32_t cp,double size,Face face,void *context,Error &e) {
    void *font=nullptr;uint32_t index=0;
    if(!select(cp,size,face,font,index,e)) return false;
    FT_GlyphSlot glyph=static_cast<FT_Face>(font)->glyph;
    if(glyph->format!=FT_GLYPH_FORMAT_OUTLINE) return e.set(Code::Font,"Glyph has no vector outline"),false;
    OutlineContext c{static_cast<cairo_t *>(context),0,0,false};
    FT_Outline_Funcs funcs{move_to,line_to,conic_to,cubic_to,0,0};
    if(FT_Outline_Decompose(&glyph->outline,&funcs,&c)) return e.set(Code::Font,"Invalid glyph outline"),false;
    if(c.started)cairo_close_path(c.cr);
    return cairo_status(c.cr)==CAIRO_STATUS_SUCCESS;
}
}
