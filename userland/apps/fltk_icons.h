#ifndef RELIEFOS_FLTK_ICONS_H
#define RELIEFOS_FLTK_ICONS_H
#include "app_icon_pixels.h"
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Window.H>
#include <reliefos/layout.h>

namespace reliefos_fltk {
inline void app_icons(const char *app) {
    if(!app || !*app) return;
    for(const char *p=app;*p;++p) if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-')) return;
    char path[512],identity[128];
    int length=snprintf(identity,sizeof(identity),"reliefos-%s",app);
    if(length<=0 || length>=int(sizeof(identity))) return;
    Fl_Window::default_xclass(identity);
    length=snprintf(path,sizeof(path),RELIEFOS_LAYOUT_RELIEFOS_APPS "/%s/%s.bmp",app,app);
    if(length<=0 || length>=int(sizeof(path))) return;
    unsigned char rgba[32*32*4],small[16*16*4],medium[48*48*4],large[64*64*4];
    if(!read_app_icon(path,rgba)) return;
    scale_app_icon(rgba,small,16); scale_app_icon(rgba,medium,48); scale_app_icon(rgba,large,64);
    Fl_RGB_Image i16(small,16,16,4),i32(rgba,32,32,4),i48(medium,48,48,4),i64(large,64,64,4);
    const Fl_RGB_Image *images[]={&i16,&i32,&i48,&i64};
    // FLTK copies these images and publishes _NET_WM_ICON / legacy WM_HINTS.
    Fl_Window::default_icons(images,4);
}
}
#endif
