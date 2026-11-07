#include "worker/font.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char **argv) {
    assert(argc==3);
    rm::FontBook fonts; rm::Error e; rm::GlyphMetrics m;
    assert(fonts.open(argv[1],argv[2],e));
    assert(fonts.measure('x',32,rm::Face::Math,m,e));
    assert(m.advance>0 && m.ascent>0);
    assert(fonts.measure(0x4e2d,32,rm::Face::Text,m,e));
    assert(m.advance>0 && m.ascent>0);
    assert(!fonts.measure(0x10ffff,32,rm::Face::Text,m,e));
    puts("font metrics and missing-glyph rejection passed");
}
