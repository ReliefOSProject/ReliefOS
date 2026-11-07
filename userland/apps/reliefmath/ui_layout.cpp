#include "ui_layout.h"
namespace rm {
OfficeLayout OfficeLayout::compute(int w,int h,bool left,bool right) {
    OfficeLayout p;p.menu={0,0,w,34};p.ribbon={0,34,w,100};
    p.top_stretch={474,0,w-682,34};p.quick_actions={w-204,4,194,26};
    p.status={0,h-28,w,28};
    int top=140,bottom=h-34,lh=bottom-top;
    int lw=left?200:0;int available=w-lw-(left?6:0);
    int ew=right?available/2:available;
    p.list={0,top,lw,lh};p.editor={lw+(left?6:0),top,ew,lh};
    p.preview={p.editor.x+ew+(right?6:0),top,right?available-ew-6:0,lh};
    if(left){
        p.list_content={8,182,lw-16,lh-86};
        p.list_actions={8,top+lh-31,184,26};
    }
    return p;
}
}
