#include "ui_layout.h"
#include <assert.h>
#include <stdio.h>
int main() {
    const int widths[]={960,1180,1600}, heights[]={600,780,900};
    for(int w: widths)for(int h: heights)
        for(int list=0;list<2;list++)for(int preview=0;preview<2;preview++) {
            rm::OfficeLayout p=rm::OfficeLayout::compute(w,h,list,preview);
            assert(p.menu.height==34&&p.ribbon.height==100);
            assert(p.editor.width>=240&&p.editor.height>0);
            assert(p.status.y+p.status.height==h);
            if(list)assert(p.list.x+p.list.width<=p.editor.x);
            if(preview)assert(p.editor.x+p.editor.width<=p.preview.x);
            assert(p.editor.x+p.editor.width<=w);

            assert(p.quick_actions.x+p.quick_actions.width==w-10);
            assert(p.top_stretch.x+p.top_stretch.width+4==p.quick_actions.x);
            for(int n=0;n<3;n++) {
                rm::Rect button={p.quick_actions.x+n*66,p.quick_actions.y,62,26};
                assert(button.x>=0&&button.x+button.width<=w);
            }
            if(list) {
                assert(p.list_content.x==p.list.x+8);
                assert(p.list_content.y>=p.list.y);
                assert(p.list_actions.x==p.list.x+8);
                assert(p.list_actions.y+p.list_actions.height<=p.list.y+p.list.height);
                assert(p.list_actions.y==p.list.y+p.list.height-31);
                const int widths[]={58,58,60};
                for(int n=0;n<3;n++) {
                    rm::Rect button={p.list_actions.x+n*62,p.list_actions.y,widths[n],26};
                    assert(button.x>=p.list.x&&button.x+button.width<=p.list.x+p.list.width);
                    assert(button.y>=p.list.y&&button.y+button.height<=p.list.y+p.list.height);
                }
            }
        }
    puts("Office layout bounds passed");
}
