#ifndef RELIEFMATH_FONT_H
#define RELIEFMATH_FONT_H
#include "../error.h"
namespace rm {
enum class Face : uint8_t { Math, Text, Italic, Bold, Blackboard, Calligraphic };
struct GlyphMetrics { double advance=0,ascent=0,descent=0,left=0; };
class FontBook {
    void *library_=nullptr,*math_=nullptr,*text_=nullptr;
public:
    FontBook()=default;
    FontBook(const FontBook &)=delete;
    FontBook &operator=(const FontBook &)=delete;
    ~FontBook();
    bool open(const char *math_path,const char *text_path,Error &);
    bool measure(uint32_t,double,Face,GlyphMetrics &,Error &);
    bool outline(uint32_t,double,Face,void *cairo_context,Error &);
private:
    bool select(uint32_t,double,Face,void *&,uint32_t &,Error &);
};
}
#endif
