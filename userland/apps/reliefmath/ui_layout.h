#ifndef RELIEFMATH_UI_LAYOUT_H
#define RELIEFMATH_UI_LAYOUT_H
namespace rm {
struct Rect { int x=0,y=0,width=0,height=0; };
struct OfficeLayout {
    Rect menu,ribbon,top_stretch,quick_actions,list,list_content,list_actions,editor,preview,status;
    static OfficeLayout compute(int width,int height,bool show_list,bool show_preview);
};
}
#endif
