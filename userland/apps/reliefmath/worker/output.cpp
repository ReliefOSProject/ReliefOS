#include "renderer.h"
#include "../limits.h"
#include "../protocol.h"
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
namespace rm {
static bool append_output(Bytes &b,const void *data,size_t length,uint64_t limit,
                          const char *limit_message,Error &e) {
    if(b.size()>limit||length>limit-b.size())
        return e.set(Code::Limit,limit_message),false;
    if(b.append(data,length))return true;
    return e.set(Code::NoMemory,"Cannot allocate formula output"),false;
}
static bool append_svg(Bytes &b,const char *s,size_t length,Error &e) {
    return append_output(b,s,length,kMaxSvgBytes,"SVG exceeds protocol byte limit",e);
}
static bool append_svg(Bytes &b,const char *s,Error &e) {
    return append_svg(b,s,strlen(s),e);
}
static bool command(Bytes &b,const char *op,const double *v,int count,Error &e) {
    if(!append_svg(b,op,e))return false;
    for(int n=0;n<count;n++) {
        char t[80]; if(!isfinite(v[n]))return e.set(Code::Limit,"Invalid path coordinate"),false;
        int len=snprintf(t,sizeof t,"%.5g ",v[n]);
        if(len<0||len>=int(sizeof t))return e.set(Code::Limit,"Invalid path coordinate"),false;
        if(!append_svg(b,t,size_t(len),e))return false;
    } return true;
}
struct PngWriter {Bytes &bytes;Error &error;};
static cairo_status_t write_png(void *p,const unsigned char *data,unsigned length) {
    PngWriter &writer=*static_cast<PngWriter *>(p);
    if(append_output(writer.bytes,data,length,kMaxPngBytes,"PNG exceeds protocol byte limit",writer.error))
        return CAIRO_STATUS_SUCCESS;
    return writer.error.code==Code::Limit?CAIRO_STATUS_WRITE_ERROR:CAIRO_STATUS_NO_MEMORY;
}
static bool raster_extent(double width,double height,uint32_t &w,uint32_t &h,Error &e) {
    width=ceil(width);height=ceil(height);
    if(width<1)width=1;
    if(height<1)height=1;
    if(!isfinite(width)||!isfinite(height)||width>kMaxRasterDimension||height>kMaxRasterDimension||
       !valid_raster_extent(uint32_t(width),uint32_t(height)))
        return e.set(Code::Limit,"Formula exceeds image dimensions or pixel limit"),false;
    w=uint32_t(width);h=uint32_t(height);return true;
}
bool output_scene(FontBook &fonts,const Box &box,const Snapshot &s,Rendered &out,Error &e) {
    const double pad=s.formula.style.padding*s.dpi/72.0*s.scale;
    uint32_t w=0,h=0;
    // These metrics are a lower bound for the later path extent. Reject huge
    // transforms before allocating or recording their Cairo outlines.
    if(!raster_extent(box.width+pad*2,box.ascent+box.descent+pad*2,w,h,e))return false;
    cairo_surface_t *record=cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA,nullptr);
    cairo_t *pathcr=cairo_create(record); bool okay=true;
    for(size_t i=0;i<box.draws.size()&&okay;i++) {
        const Draw &d=box.draws[i];
        if(d.kind==Draw::Kind::Rule)cairo_rectangle(pathcr,d.x,d.y,d.width,d.height);
        else {
            cairo_save(pathcr);cairo_translate(pathcr,d.x,d.y);cairo_scale(pathcr,d.xscale,d.yscale);
            okay=fonts.outline(d.codepoint,d.size,d.face,pathcr,e);cairo_restore(pathcr);
        }
    }
    double x1=0,y1=0,x2=0,y2=0;cairo_path_extents(pathcr,&x1,&y1,&x2,&y2);
    if(x1>0)x1=0;
    if(x2<box.width)x2=box.width;
    if(y1>-box.ascent)y1=-box.ascent;
    if(y2<box.descent)y2=box.descent;
    if(!raster_extent(x2-x1+pad*2,y2-y1+pad*2,w,h,e))okay=false;
    cairo_path_t *path=cairo_copy_path(pathcr);
    if(path->status!=CAIRO_STATUS_SUCCESS||cairo_status(pathcr)!=CAIRO_STATUS_SUCCESS) {
        okay=false;e.set(Code::NoMemory,"Cannot create formula outline");
    }
    if(!okay){cairo_path_destroy(path);cairo_destroy(pathcr);cairo_surface_destroy(record);return false;}
    uint32_t color=s.formula.style.rgba;
    double ox=pad-x1,oy=pad-y1;out.baseline=oy;
    if(s.artifacts&WantSvg) {
        char header[512];int length=snprintf(header,sizeof header,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" height=\"%u\" viewBox=\"0 0 %u %u\"><g transform=\"translate(%.8g %.8g)\"><path fill=\"#%06x\" fill-opacity=\"%.8g\" d=\"",
            w,h,w,h,ox,oy,unsigned(color>>8),(color&255)/255.0);
        if(length<=0||length>=int(sizeof header))
            okay=false,e.set(Code::Limit,"Invalid SVG header");
        else okay=append_svg(out.svg,header,size_t(length),e);
        for(int i=0;i<path->num_data&&okay;) {
            const cairo_path_data_t &d=path->data[i];double v[6]{};int count=0;const char *op="";
            if(d.header.type==CAIRO_PATH_MOVE_TO){op="M";count=2;}
            else if(d.header.type==CAIRO_PATH_LINE_TO){op="L";count=2;}
            else if(d.header.type==CAIRO_PATH_CURVE_TO){op="C";count=6;}
            else if(d.header.type==CAIRO_PATH_CLOSE_PATH){op="Z";count=0;}
            for(int k=0;k<count/2;k++){v[k*2]=path->data[i+1+k].point.x;v[k*2+1]=path->data[i+1+k].point.y;}
            okay=command(out.svg,op,v,count,e);i+=d.header.length;
        }
        if(okay)okay=append_svg(out.svg,"\"/></g></svg>\n",e);
    }
    if(okay&&(s.artifacts&(WantRaster|WantPng))) {
        cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,int(w),int(h));
        cairo_t *cr=cairo_create(surface);cairo_translate(cr,ox,oy);cairo_append_path(cr,path);
        cairo_set_source_rgba(cr,(color>>24)/255.0,((color>>16)&255)/255.0,((color>>8)&255)/255.0,(color&255)/255.0);
        cairo_fill(cr);cairo_surface_flush(surface);
        okay=cairo_status(cr)==CAIRO_STATUS_SUCCESS&&cairo_surface_status(surface)==CAIRO_STATUS_SUCCESS;
        if(okay&&(s.artifacts&WantPng)) {
            PngWriter writer{out.png,e};
            okay=cairo_surface_write_to_png_stream(surface,write_png,&writer)==CAIRO_STATUS_SUCCESS;
        }
        if(okay&&(s.artifacts&WantRaster)) {
            out.raster.width=w;out.raster.height=h;out.raster.stride=w*4;
            okay=out.raster.rgba.resize(size_t(w)*h*4);
            if(okay) {
                const unsigned char *data=cairo_image_surface_get_data(surface);
                int stride=cairo_image_surface_get_stride(surface);uint8_t *rgba=out.raster.rgba.data();
                for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++) {
                    uint32_t pixel;memcpy(&pixel,data+size_t(y)*stride+x*4,4);
                    unsigned a=pixel>>24,r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                    size_t at=(size_t(y)*w+x)*4;
                    rgba[at]=uint8_t(a?(r*255+a/2)/a:0);rgba[at+1]=uint8_t(a?(g*255+a/2)/a:0);
                    rgba[at+2]=uint8_t(a?(b*255+a/2)/a:0);rgba[at+3]=uint8_t(a);
                }
            }
        }
        cairo_destroy(cr);cairo_surface_destroy(surface);
    }
    cairo_path_destroy(path);cairo_destroy(pathcr);cairo_surface_destroy(record);
    if(!okay&&e.code==Code::None)e.set(Code::NoMemory,"Cannot produce formula image");
    return okay;
}
}
