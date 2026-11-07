#ifndef RELIEFMATH_EDITOR_VIEW_H
#define RELIEFMATH_EDITOR_VIEW_H
#include <FL/Fl_Text_Editor.H>
namespace rm {
class MathWindow;
class SourceEditor : public Fl_Text_Editor {
    MathWindow *owner_;
public:
    SourceEditor(int,int,int,int,MathWindow*);
    int handle(int) override;
};
}
#endif
