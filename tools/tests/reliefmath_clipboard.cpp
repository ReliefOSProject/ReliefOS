// Independent target clipboard consumer; leaves no window over the application.
#include "fltk_support.h"
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Window.H>
#include <FL/x.H>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
class Receiver:public Fl_Widget {
public:
 void draw()override{}
 Receiver():Fl_Widget(0,0,1,1){}
 int handle(int event)override{
  if(event==FL_PASTE&&Fl::event_clipboard_type()==Fl::clipboard_plain_text){
   const char *text=Fl::event_text();int length=Fl::event_length();
   printf("RM_CLIPBOARD_TEXT_HEX ");
   for(int n=0;n<length;n++)printf("%02x",static_cast<unsigned char>(text[n]));
   puts("");fflush(stdout);return 1;
  }
  if(event==FL_PASTE&&Fl::event_clipboard_type()==Fl::clipboard_image){
   auto *image=static_cast<Fl_RGB_Image*>(Fl::event_clipboard());
   if(image&&image->w()>0&&image->h()>0&&image->d()>=3){
    FILE *f=fopen("/home/test/clipboard.ppm","wb");if(f){fprintf(f,"P6\n%d %d\n255\n",image->w(),image->h());const unsigned char *data=reinterpret_cast<const unsigned char*>(image->data()[0]);int stride=image->ld()?image->ld():image->w()*image->d();for(int y=0;y<image->h();y++)for(int x=0;x<image->w();x++)fwrite(data+y*stride+x*image->d(),1,3,f);fclose(f);puts("RM_CLIPBOARD_IMAGE_PASS");fflush(stdout);}
   }
   delete image;return 1;
  }
  return 0;
 }
};
int main(){
 reliefos_fltk::init();fl_open_display();
 // X11 selection requests in FLTK require a mapped first_window(). This
 // override-redirect test window never takes focus or covers the editor.
 Fl_Window window(0,0,1,1);window.border(0);window.set_override();Receiver receiver;window.end();window.show();Fl::check();
 const char *text="中文公式";Fl::copy(text,int(strlen(text)),1);puts("RM_CLIPBOARD_READY");fflush(stdout);
 Window initial_owner=XGetSelectionOwner(fl_display,XInternAtom(fl_display,"CLIPBOARD",False));
 unsigned polls=0;bool copied=false;time_t until=time(nullptr)+1800;
 while(time(nullptr)<until){
  Fl::wait(.05);Window owner=XGetSelectionOwner(fl_display,XInternAtom(fl_display,"CLIPBOARD",False));
  // Do not run another synchronous TARGETS probe while a paste request is
  // pending: clipboard_contains consumes X events, including that response.
  if(++polls%10==0&&owner!=initial_owner&&owner!=0){
   if(!copied&&Fl::clipboard_contains(Fl::clipboard_image)){copied=true;Fl::paste(receiver,1,Fl::clipboard_image);}
   else if(Fl::clipboard_contains(Fl::clipboard_plain_text))Fl::paste(receiver,1,Fl::clipboard_plain_text);
  }
  usleep(10000);
 }
 return 0;
}
