#ifndef RELIEFOS_APP_ICON_PIXELS_H
#define RELIEFOS_APP_ICON_PIXELS_H
#include <stdint.h>
#include <stdio.h>

namespace reliefos_fltk {
inline uint32_t icon_u32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
// Private build artwork is a bounded, uncompressed 32x32 BGRA BMP. Public
// desktop icons are PNGs in hicolor; this reader preserves the source alpha
// without adding an image codec or changing document/image-viewer decoding.
inline bool read_app_icon(const char *path,unsigned char *rgba) {
    FILE *file=fopen(path,"rb"); if(!file) return false;
    unsigned char header[54],bgra[32*32*4];
    bool ok=fread(header,1,sizeof(header),file)==sizeof(header);
    if(ok) ok=header[0]=='B' && header[1]=='M' && icon_u32(header+2)==sizeof(header)+sizeof(bgra) &&
        icon_u32(header+10)==54 && icon_u32(header+14)==40 && icon_u32(header+18)==32 &&
        (icon_u32(header+22)==32 || icon_u32(header+22)==0xffffffe0u) &&
        header[26]==1 && !header[27] && header[28]==32 && !header[29] && !icon_u32(header+30);
    if(ok) ok=fread(bgra,1,sizeof(bgra),file)==sizeof(bgra) && fgetc(file)==EOF && !ferror(file);
    if(fclose(file)) ok=false;
    if(!ok) return false;
    for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x) {
        unsigned source=((icon_u32(header+22)==32?31-y:y)*32+x)*4, target=(y*32+x)*4;
        rgba[target]=bgra[source+2]; rgba[target+1]=bgra[source+1];
        rgba[target+2]=bgra[source]; rgba[target+3]=bgra[source+3];
    }
    return true;
}
inline void scale_app_icon(const unsigned char *rgba,unsigned char *output,unsigned size) {
    for(unsigned y=0;y<size;++y) for(unsigned x=0;x<size;++x) {
        unsigned source=((y*32/size)*32+x*32/size)*4,target=(y*size+x)*4;
        for(unsigned channel=0;channel<4;++channel) output[target+channel]=rgba[source+channel];
    }
}
}
#endif
