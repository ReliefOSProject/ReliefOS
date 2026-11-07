#include "preview.h"
#include <FL/fl_draw.H>
#include <math.h>
namespace rm {
namespace {
struct Scan {const Raster *r;double scale,ox,oy;};
void scan(void *v,int x,int y,int width,unsigned char *out) {
    const Scan &s=*static_cast<Scan*>(v);const Raster &r=*s.r;
    for(int n=0;n<width;n++) {
        int sx=int(floor((x+n-s.ox)/s.scale)),sy=int(floor((y-s.oy)/s.scale));
        unsigned background=(((x+n)/12+y/12)&1)?244:255;
        unsigned red=background,green=background,blue=background;
        if(sx>=0&&sy>=0&&uint32_t(sx)<r.width&&uint32_t(sy)<r.height) {
            const uint8_t *p=r.rgba.data()+size_t(sy)*r.stride+size_t(sx)*4;
            unsigned a=p[3];red=(p[0]*a+background*(255-a))/255;
            green=(p[1]*a+background*(255-a))/255;blue=(p[2]*a+background*(255-a))/255;
        }
        out[n*3]=uint8_t(red);out[n*3+1]=uint8_t(green);out[n*3+2]=uint8_t(blue);
    }
}
}
void Preview::draw() {
    fl_push_clip(x(),y(),w(),h());
    fl_color(FL_BACKGROUND2_COLOR);fl_rectf(x(),y(),w(),h());
    if(raster_.width&&raster_.height) {
        double scale=zoom_;
        if(fit_) {scale=(w()-32.)/raster_.width;double sy=(h()-32.)/raster_.height;if(sy<scale)scale=sy;if(scale>1.5)scale=1.5;}
        if(scale<.0001)scale=.0001;
        Scan s{&raster_,scale,(w()-raster_.width*scale)/2.,(h()-raster_.height*scale)/2.};
        fl_draw_image(scan,&s,x(),y(),w(),h(),3);
    }else{
        fl_color(FL_FOREGROUND_COLOR);fl_font(FL_HELVETICA,15);
        fl_draw("LaTeX",x(),y(),w(),h(),FL_ALIGN_CENTER);
    }
    fl_pop_clip();
}
}
