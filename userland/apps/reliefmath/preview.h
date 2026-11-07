#ifndef RELIEFMATH_PREVIEW_H
#define RELIEFMATH_PREVIEW_H
#include <FL/Fl_Widget.H>
#include "render_types.h"
namespace rm {
class Preview : public Fl_Widget {
    Raster raster_;
    double zoom_=1;
    bool fit_=true;
public:
    Preview(int x,int y,int w,int h):Fl_Widget(x,y,w,h){}
    void set_raster(Raster &&r){raster_=static_cast<Raster&&>(r);redraw();}
    void clear(){raster_=Raster{};redraw();}
    void zoom(double z){zoom_=z;fit_=false;redraw();}
    void fit(){fit_=true;redraw();}
    const Raster &raster()const{return raster_;}
    void draw() override;
};
}
#endif
