#include "window.h"
#include "view.h"
#include "../fltk_support.h"
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Value_Input.H>
#include <math.h>
#include <stdio.h>
#include <string.h>
namespace rw {
namespace {
bool search_escape(int event) {
    return (event==FL_KEYDOWN||event==FL_SHORTCUT)&&Fl::event_key()==FL_Escape;
}
class SearchWindow : public Fl_Window {
public:
    using Fl_Window::Fl_Window;
    int handle(int event) override {
        if(search_escape(event)) { do_callback(); return 1; }
        return Fl_Window::handle(event);
    }
};
class SearchInput : public Fl_Input {
public:
    using Fl_Input::Fl_Input;
    int handle(int event) override {
        // FLTK dispatches keys to the focused input before its window. Make
        // cancellation explicit here as well as for buttons in SearchWindow.
        if(search_escape(event)) { window()->do_callback(); return 1; }
        return Fl_Input::handle(event);
    }
};
void field(Fl_Widget &widget) { widget.align(FL_ALIGN_TOP_LEFT); }
void number(Fl_Value_Input &input,double value,double low,double high,double step=1) { input.value(value); input.bounds(low,high); input.step(step); field(input); }
bool valid_number(const Fl_Value_Input &input,double low,double high) {
    double value=input.value(); return isfinite(value)&&value>=low&&value<=high;
}
void close_dialog(Fl_Widget *widget,void *) { widget->window()->hide(); }
void wait_dialog(Fl_Window &window) {
    reliefos_fltk::style(&window); window.set_modal();
    int X,Y,W,H; Fl::screen_work_area(X,Y,W,H); window.position(X+(W-window.w())/2,Y+(H-window.h())/2);
    window.show(); while(window.shown()) Fl::wait();
}
}
void WordWindow::page_dialog() {
    const PageSettings &page=editor.doc.page;
    Fl_Window dialog(480,500,T("Page setup")); int accepted=0;
    Fl_Choice paper(22,40,205,28,T("Paper size")); field(paper); paper.add("A4"); paper.add("Letter"); paper.add(T("Custom"));
    unsigned short_edge=page.width<page.height?page.width:page.height,long_edge=page.width>page.height?page.width:page.height;
    paper.value(short_edge==595&&long_edge==842?0:short_edge==612&&long_edge==792?1:2);
    Fl_Choice orientation(253,40,205,28,T("Orientation")); field(orientation); orientation.add(T("Portrait")); orientation.add(T("Landscape")); orientation.value(page.width>page.height);
    Fl_Value_Input width(22,100,205,28,T("Width (mm)")),height(253,100,205,28,T("Height (mm)"));
    number(width,page.width*25.4/72,50,1000,.1); number(height,page.height*25.4/72,50,1000,.1);
    Fl_Value_Input left(22,166,205,28,T("Left margin (mm)")),right(253,166,205,28,T("Right margin (mm)"));
    Fl_Value_Input top(22,222,205,28,T("Top margin (mm)")),bottom(253,222,205,28,T("Bottom margin (mm)"));
    number(left,page.left*25.4/72,0,100,.1); number(right,page.right*25.4/72,0,100,.1);
    number(top,page.top*25.4/72,0,100,.1); number(bottom,page.bottom*25.4/72,0,100,.1);
    Fl_Input header(22,284,436,28,T("Header")); field(header); header.value(page.header.c_str()); header.maximum_size(512);
    Fl_Input footer(22,342,436,28,T("Footer ({page} inserts the page number)")); field(footer); footer.value(page.footer.c_str()); footer.maximum_size(512);
    Fl_Check_Button numbers(22,389,436,28,T("Show page numbers")); numbers.value(page.page_numbers);
    Fl_Button cancel(268,454,90,30,T("Cancel")); cancel.callback(close_dialog);
    Fl_Return_Button apply(368,454,90,30,T("Apply"));
    apply.callback([](Fl_Widget *widget,void *context){ *static_cast<int *>(context)=1; widget->window()->hide(); },&accepted);
    dialog.end();
    struct Sizes { Fl_Value_Input *width,*height; } sizes{&width,&height};
    auto availability=[](Fl_Widget *widget,void *context) {
        auto *fields=static_cast<Sizes *>(context); bool custom=static_cast<Fl_Choice *>(widget)->value()==2;
        if(custom) { fields->width->activate(); fields->height->activate(); } else { fields->width->deactivate(); fields->height->deactivate(); }
    };
    paper.callback(availability,&sizes); availability(&paper,&sizes);
    for(;;) {
        accepted=0; wait_dialog(dialog); if(!accepted) break;
        if(!valid_number(width,50,1000)||!valid_number(height,50,1000)||
           !valid_number(left,0,100)||!valid_number(right,0,100)||
           !valid_number(top,0,100)||!valid_number(bottom,0,100)) {
            fl_alert("%s",T("Enter finite page values within the displayed ranges")); continue;
        }
        PageSettings candidate=page;
        candidate.width=uint16_t(round(width.value()*72/25.4)); candidate.height=uint16_t(round(height.value()*72/25.4));
        if(paper.value()==0) { candidate.width=595; candidate.height=842; }
        else if(paper.value()==1) { candidate.width=612; candidate.height=792; }
        if((orientation.value()==0&&candidate.width>candidate.height)||(orientation.value()==1&&candidate.width<candidate.height)) { uint16_t n=candidate.width; candidate.width=candidate.height; candidate.height=n; }
        candidate.left=uint16_t(round(left.value()*72/25.4)); candidate.right=uint16_t(round(right.value()*72/25.4));
        candidate.top=uint16_t(round(top.value()*72/25.4)); candidate.bottom=uint16_t(round(bottom.value()*72/25.4)); candidate.page_numbers=numbers.value();
        if(!candidate.header.set(header.value())||!candidate.footer.set(footer.value())) { fl_alert("%s",T("Not enough memory")); continue; }
        if(editor.set_page(candidate)) { changed(); break; }
        fl_alert("%s",T(editor.error.c_str()));
    }
    view->take_focus();
}
void WordWindow::paragraph_dialog() {
    ParagraphStyle style=editor.doc.paragraphs[editor.doc.paragraph_at(editor.caret)].style;
    Fl_Window dialog(464,375,T("Paragraph")); int accepted=0;
    Fl_Choice alignment(22,40,198,28,T("Alignment")); field(alignment); alignment.add(T("Left")); alignment.add(T("Center")); alignment.add(T("Right")); alignment.add(T("Justify")); alignment.value(int(style.alignment));
    Fl_Choice list(244,40,198,28,T("List")); field(list); list.add(T("None")); list.add(T("Bullets")); list.add(T("Numbering")); list.value(int(style.list));
    Fl_Value_Input left(22,105,198,28,T("Left indent (mm)")),right(244,105,198,28,T("Right indent (mm)"));
    number(left,style.left_indent*25.4/72,0,100,.1); number(right,style.right_indent*25.4/72,0,100,.1);
    Fl_Value_Input first(22,167,198,28,T("First line indent (mm)")),spacing(244,167,198,28,T("Line spacing (%)"));
    number(first,style.first_indent*25.4/72,-50,100,.1); number(spacing,style.line_spacing,100,300,5);
    Fl_Value_Input before(22,229,198,28,T("Space before (pt)")),after(244,229,198,28,T("Space after (pt)"));
    number(before,style.before,0,144); number(after,style.after,0,144);
    Fl_Button cancel(244,323,92,30,T("Cancel")); cancel.callback(close_dialog);
    Fl_Return_Button apply(350,323,92,30,T("Apply")); apply.callback([](Fl_Widget *widget,void *context){ *static_cast<int *>(context)=1; widget->window()->hide(); },&accepted); dialog.end();
    for(;;) {
        accepted=0; wait_dialog(dialog); if(!accepted) break;
        if(!valid_number(left,0,100)||!valid_number(right,0,100)||
           !valid_number(first,-50,100)||!valid_number(spacing,100,300)||
           !valid_number(before,0,144)||!valid_number(after,0,144)) {
            fl_alert("%s",T("Enter finite paragraph values within the displayed ranges")); continue;
        }
        style.alignment=Alignment(alignment.value()); style.list=ListKind(list.value());
        style.left_indent=int16_t(round(left.value()*72/25.4)); style.right_indent=int16_t(round(right.value()*72/25.4)); style.first_indent=int16_t(round(first.value()*72/25.4));
        style.before=uint16_t(before.value()); style.after=uint16_t(after.value()); style.line_spacing=uint16_t(spacing.value());
        if(editor.apply_paragraph(style)) { changed(); break; } fl_alert("%s",T(editor.error.c_str()));
    }
    view->take_focus();
}
void WordWindow::search_dialog(bool replace) {
    if(!search_window_) {
        search_window_=new SearchWindow(440,240,T("Find and replace")); search_window_->begin();
        find_=new SearchInput(90,22,330,28,T("Find:")); find_->maximum_size(2048);
        replacement_=new SearchInput(90,65,330,28,T("Replace:")); replacement_->maximum_size(2048);
        match_case_=new Fl_Check_Button(90,106,330,26,T("Match case"));
        Fl_Button *next=new Fl_Button(20,152,124,30,T("Find next"));
        next->callback([](Fl_Widget *,void *context){ static_cast<WordWindow *>(context)->act(Command::FindNext); },this);
        Fl_Button *one=new Fl_Button(158,152,124,30,T("Replace"));
        one->callback([](Fl_Widget *,void *context){ static_cast<WordWindow *>(context)->act(Command::ReplaceOne); },this);
        Fl_Button *all=new Fl_Button(296,152,124,30,T("Replace all"));
        all->callback([](Fl_Widget *,void *context){ static_cast<WordWindow *>(context)->act(Command::ReplaceAll); },this);
        Fl_Button *close=new Fl_Button(328,200,92,28,T("Close"));
        close->callback([](Fl_Widget *widget,void *context){
            auto *owner=static_cast<WordWindow *>(context); widget->window()->hide();
            if(owner->view) owner->view->take_focus();
        },this);
        find_->when(FL_WHEN_ENTER_KEY_ALWAYS); find_->callback([](Fl_Widget *,void *context){ static_cast<WordWindow *>(context)->find_next(); },this);
        search_window_->end(); reliefos_fltk::style(search_window_); search_window_->set_non_modal();
        search_window_->callback([](Fl_Widget *widget,void *context){
            auto *owner=static_cast<WordWindow *>(context); widget->hide();
            if(owner->view) owner->view->take_focus();
        },this);
    }
    search_window_->position(x()+w()-search_window_->w()-20,y()+115); search_window_->show();
    if(replace) replacement_->show(); else replacement_->show();
    find_->take_focus(); find_->position(0,find_->size());
}
void WordWindow::find_next() {
    if(!find_||!*find_->value()) { search_dialog(false); message(T("Enter search text")); return; }
    size_t a=0,b=0; bool found=editor.find(find_->value(),match_case_->value(),editor.selection_end(),a,b);
    if(!found) found=editor.find(find_->value(),match_case_->value(),0,a,b);
    if(!found) { message(T("Text not found")); return; }
    editor.select(a,b); view->ensure_caret(); view->redraw(); selected(); message(T("Match selected"));
}
void WordWindow::replace_one() {
    if(!find_||!*find_->value()) { message(T("Enter search text")); return; }
    size_t a=0,b=0;
    bool exact=editor.find(find_->value(),match_case_->value(),editor.selection_start(),a,b)&&a==editor.selection_start()&&b==editor.selection_end();
    if(exact) {
        if(!editor.insert_utf8(replacement_->value(),strlen(replacement_->value()))) { message(editor.error.c_str()); return; }
        changed();
    }
    find_next();
    if(search_window_&&search_window_->shown()) find_->take_focus();
}
void WordWindow::replace_all() {
    if(!find_||!*find_->value()) { message(T("Enter search text")); return; }
    unsigned count=0;
    if(!editor.replace_all(find_->value(),replacement_->value(),match_case_->value(),count)) { message(editor.error.c_str()); return; }
    changed(); char text[128]; snprintf(text,sizeof(text),T("Replaced %u matches"),count); message(text);
    if(search_window_&&search_window_->shown()) find_->take_focus();
}
}
