#include "editor_view.h"
#include "window.h"
#include <FL/Fl.H>
namespace rm {
SourceEditor::SourceEditor(int x,int y,int w,int h,MathWindow *owner):Fl_Text_Editor(x,y,w,h),owner_(owner){}
int SourceEditor::handle(int event){
    if(event==FL_KEYDOWN){
        int key=Fl::event_key();bool ctrl=(Fl::event_state()&FL_CTRL)!=0,shift=(Fl::event_state()&FL_SHIFT)!=0;
        if(key==FL_Tab){if(owner_->placeholder(shift))return 1;return 0;}
        if(ctrl){switch(key){case 'z':owner_->run(shift?Command::Redo:Command::Undo);return 1;case 'y':owner_->run(Command::Redo);return 1;case 's':owner_->run(shift?Command::SaveAs:Command::Save);return 1;case 'n':owner_->run(Command::New);return 1;case 'o':owner_->run(Command::Open);return 1;case 'f':owner_->run(Command::Find);return 1;case 'h':owner_->run(Command::Replace);return 1;case ' ':owner_->run(Command::Complete);return 1;default:break;}}
    }
    return Fl_Text_Editor::handle(event);
}
}
