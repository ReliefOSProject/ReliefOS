#ifndef RELIEFWORD_LAYOUT_H
#define RELIEFWORD_LAYOUT_H
#include "model.h"
namespace rw {
struct Metrics { float advance=6,ascent=10,descent=3; };
using Measure=Metrics (*)(void *,uint32_t,const TextStyle &);
struct Line {
    size_t start=0,end=0,paragraph=0;
    unsigned page=0,list_number=0;
    float x=0,y=0,width=0,height=0,baseline=0;
    bool paragraph_start=false,paragraph_end=false;
};
struct GlyphBox { float x=0,y=0,width=0; unsigned line=0; };
class Layout {
public:
    Array<Line> lines;
    Array<GlyphBox> boxes; // one per character plus document-end caret
    Array<uint8_t> grapheme_boundaries; // one per caret offset; nonzero means legal
    unsigned pages=1;
    Text error;
    bool build(const Document &doc,Measure measure,void *context=nullptr);
    size_t hit_test(unsigned page,float x,float y) const;
    size_t line_for(size_t offset,bool prefer_previous=false) const;
    float caret_x(size_t offset,size_t line) const;
};
}
#endif
