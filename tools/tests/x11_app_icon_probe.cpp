// Read actual X11 properties; compare every ARGB pixel with the installed
// application's source artwork rather than trusting a screenshot alone.
#include "app_icon_pixels.h"
#include <FL/Fl.H>
#include <FL/x.H>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool property(Window window,Atom atom,Atom type,unsigned long limit,
                     unsigned char *&data,unsigned long &count) {
    Atom actual; int format=0; unsigned long remaining=0;
    data=nullptr; count=0;
    return XGetWindowProperty(fl_display,window,atom,0,limit,False,type,&actual,&format,
                              &count,&remaining,&data)==Success && actual==type && format==32 && !remaining && data;
}
static int verify(Window window,const char *app) {
    char path[512],identity[128];
    snprintf(path,sizeof(path),"/usr/lib/reliefos/apps/%s/%s.bmp",app,app);
    snprintf(identity,sizeof(identity),"reliefos-%s",app);
    unsigned char pixels[32*32*4];
    if(!reliefos_fltk::read_app_icon(path,pixels)) return 2;
    XClassHint hint{};
    bool matches=XGetClassHint(fl_display,window,&hint) &&
        ((hint.res_name && !strcmp(hint.res_name,identity)) || (hint.res_class && !strcmp(hint.res_class,identity)));
    if(hint.res_name) XFree(hint.res_name);
    if(hint.res_class) XFree(hint.res_class);
    if(!matches) { fprintf(stderr,"APP_ICON_FAIL app=%s WM_CLASS\n",app); return 3; }
    unsigned char *data=nullptr; unsigned long count=0;
    if(!property(window,XInternAtom(fl_display,"_NET_WM_ICON",False),XA_CARDINAL,100000,data,count)) {
        if(data) XFree(data);
        fprintf(stderr,"APP_ICON_FAIL app=%s missing _NET_WM_ICON\n",app); return 4;
    }
    unsigned long *values=reinterpret_cast<unsigned long *>(data),at=0;
    unsigned seen=0;
    while(at+2<=count) {
        unsigned long w=values[at++],h=values[at++];
        int bit=w==16?0:w==32?1:w==48?2:w==64?3:-1;
        if(bit<0 || h!=w || (seen&(1u<<bit)) || w*h>count-at) { XFree(data); return 5; }
        seen|=1u<<bit;
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
            unsigned source=((y*32/h)*32+x*32/w)*4;
            uint32_t expected=uint32_t(pixels[source+3])<<24 | uint32_t(pixels[source])<<16 |
                              uint32_t(pixels[source+1])<<8 | pixels[source+2];
            if(uint32_t(values[at++])!=expected) { XFree(data); return 6; }
        }
    }
    XFree(data);
    if(at!=count || seen!=15) return 7;
    printf("APP_ICON_PASS app=%s sizes=16,32,48,64 alpha=preserved wm_class=%s\n",app,identity);
    return 0;
}
int main(int argc,char **argv) {
    if(argc!=3) return 64;
    unsigned long pid=strtoul(argv[1],nullptr,10);
    fl_open_display();
    Atom clients=XInternAtom(fl_display,"_NET_CLIENT_LIST",False),owner=XInternAtom(fl_display,"_NET_WM_PID",False);
    for(int attempt=0;attempt<100;++attempt) {
        unsigned char *list=nullptr; unsigned long count=0;
        if(property(DefaultRootWindow(fl_display),clients,XA_WINDOW,4096,list,count)) {
            Window *windows=reinterpret_cast<Window *>(list);
            for(unsigned long i=0;i<count;++i) {
                unsigned char *data=nullptr; unsigned long owners=0;
                bool found=property(windows[i],owner,XA_CARDINAL,1,data,owners) && owners==1 &&
                           *reinterpret_cast<unsigned long *>(data)==pid;
                if(data) XFree(data);
                if(found) { Window window=windows[i]; XFree(list); return verify(window,argv[2]); }
            }
        }
        if(list) XFree(list);
        usleep(100000);
    }
    fprintf(stderr,"APP_ICON_FAIL app=%s no window for pid=%lu\n",argv[2],pid);
    return 8;
}
