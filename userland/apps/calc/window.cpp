#include "window.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <stdio.h>
#include <string.h>

namespace calc {
namespace {
// Focused FLTK inputs can consume key events before the window's shortcut
// fallback. Route calculator shortcuts before normal text editing, including
// Ctrl+Enter in the multiline statistics editor.
class Input : public Fl_Input {
    Window *owner_;
public:
    Input(int x,int y,int w,int h,Window *owner,int kind=FL_NORMAL_INPUT)
        : Fl_Input(x,y,w,h),owner_(owner) { type(kind); }
    int handle(int event) override { return owner_->keyboard(event) ? 1 : Fl_Input::handle(event); }
};
class Output : public Fl_Output {
    Window *owner_;
public:
    Output(int x,int y,int w,int h,Window *owner,bool multiline)
        : Fl_Output(x,y,w,h),owner_(owner) { if (multiline) type(FL_MULTILINE_OUTPUT); }
    int handle(int event) override { return owner_->keyboard(event) ? 1 : Fl_Output::handle(event); }
};
class Key : public Fl_Button {
    Window *owner_;
    const char *token_;
    static void pressed(Fl_Widget *w, void *) { auto *key=static_cast<Key *>(w); key->owner_->key(key->token_); }
public:
    Key(int x,int y,int w,int h,const char *label,const char *token,Window *owner)
        : Fl_Button(x,y,w,h,label),owner_(owner),token_(token) {
        callback(pressed);
        labelsize(strlen(label)==1 ? 20 : 16);
    }
};
void field_label(int x,int y,int w,const char *text) {
    auto *box=new Fl_Box(x,y,w,22,text); box->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
}
void calculate_cb(Fl_Widget *,void *p) { static_cast<Window *>(p)->calculate(); }
void clear_cb(Fl_Widget *,void *p) { static_cast<Window *>(p)->clear(); }
void copy_cb(Fl_Widget *,void *p) { static_cast<Window *>(p)->copy(); }
void options_cb(Fl_Widget *,void *p) { static_cast<Window *>(p)->programmer_options(); }
Fl_Button *button(int x,int y,int w,const char *text,Fl_Callback *cb,Window *owner) {
    auto *b=new Fl_Button(x,y,w,30,text); b->callback(cb,owner); return b;
}
}

Fl_Output *Window::make_output(int x,int y,int w,int h,bool multiline) {
    auto *out=new Output(x,y,w,h,this,multiline); out->textsize(16); return out;
}

Window::Window() : Fl_Double_Window(980,680,T("Calculator")),page_x_(166),page_y_(42),page_w_(574),page_h_(600) {
    size_range(940,660);
    auto *file=new Fl_Menu_Button(8,4,72,28,T("File"));
    file->add(T("Close"),0,[](Fl_Widget *,void *p){ static_cast<Window *>(p)->menu(0); },this);
    auto *edit=new Fl_Menu_Button(82,4,72,28,T("Edit"));
    edit->add(T("Copy result"),FL_CTRL|FL_SHIFT|'c',[](Fl_Widget *,void *p){ static_cast<Window *>(p)->copy(); },this);
    edit->add(T("Clear input"),0,clear_cb,this);
    edit->add(T("Clear history"),0,[](Fl_Widget *,void *p){ static_cast<Window *>(p)->clear_history(); },this);
    auto *help=new Fl_Menu_Button(156,4,72,28,T("Help"));
    help->add(T("Calculator help"),FL_F+1,[](Fl_Widget *,void *p){ static_cast<Window *>(p)->menu(1); },this);
    auto *line=new Fl_Box(8,36,w()-16,2); line->box(FL_THIN_DOWN_BOX);

    auto *sidebar=new Fl_Group(10,page_y_,144,page_h_);
    field_label(12,48,140,T("Mode"));
    navigation_=new Fl_Hold_Browser(12,76,140,192);
    navigation_->textsize(16); navigation_->format_char(0);
    const char *modes[]={T("Standard"),T("Scientific"),T("Programmer"),T("Unit conversion"),T("Statistics"),T("Date calculation")};
    for (const char *name : modes)
        navigation_->add(name);
    navigation_->value(1);
    navigation_->callback([](Fl_Widget *w,void *p){ static_cast<Window *>(p)->select_mode(static_cast<Fl_Hold_Browser *>(w)->value()-1); },this);
    auto *shortcuts=new Fl_Box(14,292,136,120,T("Ctrl+1..6: mode\nEnter: calculate\nEsc: clear input\nF1: help"));
    shortcuts->align(FL_ALIGN_LEFT|FL_ALIGN_TOP|FL_ALIGN_INSIDE); shortcuts->labelsize(13);
    field_label(12,440,140,T("Memory")); memory_display_=make_output(12,468,140,30); memory_display_->value("0");
    auto *offline=new Fl_Box(14,520,136,64,T("All modes work\noffline."));
    offline->align(FL_ALIGN_LEFT|FL_ALIGN_TOP|FL_ALIGN_INSIDE); offline->labelsize(13);
    sidebar->end();

    content_=new Fl_Group(page_x_,page_y_,page_w_,page_h_);
    build_calculator(0); build_calculator(1); build_programmer();
    build_converter(); build_statistics(); build_dates();
    content_->resizable(content_); content_->end();

    auto *history_group=new Fl_Group(752,page_y_,216,page_h_);
    field_label(754,48,124,T("History"));
    button(892,46,74,T("Clear"),[](Fl_Widget *,void *p){ static_cast<Window *>(p)->clear_history(); },this);
    history_=new Fl_Hold_Browser(754,86,212,512); history_->textsize(13); history_->format_char(0);
    history_->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->history_selected(); },this);
    field_label(754,608,212,T("Double-click to reuse"));
    history_group->resizable(history_); history_group->end();

    status_=new Fl_Box(12,650,956,22,T("Ready"));
    status_->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE); status_->labelsize(13);
    end(); resizable(content_);
    for (int n=1;n<6;++n) pages_[n]->hide();
    category_changed(); programmer_options();
    date_today(0); date_today(1);
    select_mode(0);
}
Window::~Window() { Fl::remove_timeout(conversion_timeout,this); }

void Window::build_calculator(int mode) {
    const int x=page_x_,y=page_y_,w=page_w_;
    auto *page=pages_[mode]=new Fl_Group(x,y,w,page_h_);
    field_label(x,y+4,w,T("Expression"));
    expression_[mode]=new Input(x,y+30,w,40,this);
    expression_[mode]->maximum_size(ExpressionLimit); expression_[mode]->textsize(21);
    expression_[mode]->when(FL_WHEN_CHANGED);
    expression_[mode]->callback([](Fl_Widget *,void *p){ auto *self=static_cast<Window *>(p); self->edited(self->mode_); },this);
    answer_[mode]=make_output(x,y+80,w,44); answer_[mode]->textsize(25); answer_[mode]->value("0");
    const char *memories[]={"MC","MR","MS","M+","M-"};
    for (int i=0;i<5;++i) {
        auto *b=new Key(x+i*(w-4)/5,y+136,(w-24)/5,30,memories[i],memories[i],this);
        const char *tips[]={T("Clear memory"),T("Recall memory"),T("Store in memory"),T("Add to memory"),T("Subtract from memory")};
        b->tooltip(tips[i]);
    }
    int start=y+194;
    if (mode==1) {
        field_label(x,y+174,106,T("Angle unit"));
        angles_=new Fl_Choice(x+112,y+172,200,28);
        angles_->add(T("Degrees (DEG)")); angles_->add(T("Radians (RAD)")); angles_->value(0);
        angles_->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->calculate(false); },this);
        start=y+214;
    }
    auto *grid=new Fl_Group(x,start,w,mode==0 ? 332 : 302);
    struct Cell { const char *label,*token; };
    static const Cell standard[]={
        {"%","%"},{"ans","ans"},{"C","C"},{"BS","BS"},{"1/x","reciprocal"},
        {"x^2","^2"},{"sqrt","sqrt("},{"(","("},{")",")"},{"/","/"},
        {"7","7"},{"8","8"},{"9","9"},{"*","*"},{"mem","mem"},
        {"4","4"},{"5","5"},{"6","6"},{"-","-"},{"+/-","negate"},
        {"1","1"},{"2","2"},{"3","3"},{"+","+"},{"pi","pi"},
        {"0","0"},{".","."},{"(","("},{")",")"},{"=","="}
    };
    static const Cell scientific[]={
        {"sin","sin("},{"cos","cos("},{"tan","tan("},{"ln","ln("},{"log","log("},{"C","C"},
        {"asin","asin("},{"acos","acos("},{"atan","atan("},{"exp","exp("},{"10^x","10^"},{"BS","BS"},
        {"sinh","sinh("},{"cosh","cosh("},{"tanh","tanh("},{"sqrt","sqrt("},{"cbrt","cbrt("},{"abs","abs("},
        {"7","7"},{"8","8"},{"9","9"},{"/","/"},{"^","^"},{"!","!"},
        {"4","4"},{"5","5"},{"6","6"},{"*","*"},{"(","("},{")",")"},
        {"1","1"},{"2","2"},{"3","3"},{"-","-"},{"pi","pi"},{"e","e"},
        {"0","0"},{".","."},{"ans","ans"},{"+","+"},{",",","},{"=","="}
    };
    const Cell *cells=mode ? scientific : standard;
    int columns=mode ? 6 : 5, rows=mode ? 7 : 6, pitch=mode ? 43 : 55;
    for (int i=0;i<columns*rows;++i)
        new Key(x+(i%columns)*w/columns,start+(i/columns)*pitch,w/columns-6,pitch-7,cells[i].label,cells[i].token,this);
    grid->resizable(grid); grid->end();
    auto *hint=new Fl_Box(x,y+560,w,36,mode ? T("^ power   ! factorial   ans last answer   mem stored value") : T("Decimal arithmetic, parentheses and percentage expressions"));
    hint->align(FL_ALIGN_LEFT|FL_ALIGN_WRAP|FL_ALIGN_INSIDE); hint->labelsize(13);
    page->resizable(grid); page->end();
}

void Window::build_programmer() {
    const int x=page_x_,y=page_y_,w=page_w_;
    auto *page=pages_[2]=new Fl_Group(x,y,w,page_h_);
    base_=new Fl_Choice(x,y+8,174,30); base_->add(T("Decimal (DEC)")); base_->add(T("Hexadecimal (HEX)"));
    base_->add(T("Octal (OCT)")); base_->add(T("Binary (BIN)")); base_->value(0); base_->callback(options_cb,this);
    width_=new Fl_Choice(x+186,y+8,140,30);
    width_->add(T("8 bits")); width_->add(T("16 bits")); width_->add(T("32 bits")); width_->add(T("64 bits"));
    width_->value(3); width_->callback(options_cb,this);
    signed_=new Fl_Check_Button(x+340,y+8,w-340,30,T("Signed (two's complement)")); signed_->callback(options_cb,this);
    expression_[2]=new Input(x,y+50,w-72,36,this); expression_[2]->maximum_size(ExpressionLimit); expression_[2]->textsize(18);
    expression_[2]->when(FL_WHEN_CHANGED); expression_[2]->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->edited(2); },this);
    button(x+w-64,y+53,64,"=",calculate_cb,this);
    const char *names[]={"HEX","DEC","OCT","BIN"};
    for (int i=0;i<4;++i) {
        field_label(x,y+98+i*33,44,names[i]);
        bases_[i]=make_output(x+50,y+98+i*33,w-50,i==3 ? 48 : 28,i==3);
        bases_[i]->textsize(i==3 ? 13 : 16);
    }
    auto *grid=new Fl_Group(x,y+260,w,180);
    const char *keys[]={"A","B","C","D","E","F", "7","8","9","/","<<",">>",
                        "4","5","6","*","AND","XOR", "1","2","3","-","OR","NOT",
                        "0","(",")","+","MOD","BS"};
    for (int i=0;i<30;++i) {
        const char *token=keys[i];
        if (!strcmp(token,"AND")) token=" & ";
        if (!strcmp(token,"OR")) token=" | ";
        if (!strcmp(token,"XOR")) token=" ^ ";
        if (!strcmp(token,"MOD")) token=" % ";
        if (!strcmp(token,"NOT")) token="bitnot";
        auto *b=new Key(x+(i%6)*w/6,y+260+(i/6)*36,w/6-6,30,keys[i],token,this);
        if (strlen(keys[i])==1) {
            char c=keys[i][0]; int d=c>='0' && c<='9' ? c-'0' : c>='A' && c<='F' ? c-'A'+10 : -1;
            if (d>=0) digits_[d]=b;
        }
    }
    grid->resizable(grid); grid->end();
    field_label(x,y+450,w,T("Bit editor: high bits at left; click to toggle"));
    auto *bit_group=new Fl_Group(x,y+478,w,96);
    for (unsigned i=0;i<64;++i) {
        unsigned bit=63-i;
        auto *b=bits_[bit]=new Fl_Button(x+int(i%16)*w/16,y+478+int(i/16)*24,w/16-3,21,"0");
        b->type(FL_TOGGLE_BUTTON); b->labelsize(12);
        char tooltip[32]; snprintf(tooltip,sizeof(tooltip),"Bit %u",bit); b->copy_tooltip(tooltip);
        b->callback([](Fl_Widget *widget,void *p){
            auto *self=static_cast<Window *>(p);
            for (unsigned n=0;n<64;++n) if (self->bits_[n]==widget) { self->bit_changed(n); break; }
        },this);
    }
    bit_group->resizable(bit_group); bit_group->end();
    page->resizable(grid); page->end();
}

void Window::build_converter() {
    const int x=page_x_,y=page_y_,w=page_w_;
    auto *page=pages_[3]=new Fl_Group(x,y,w,page_h_);
    field_label(x,y+4,w,T("Category"));
    category_=new Fl_Choice(x,y+30,w,30);
    size_t count=0; auto *all=categories(&count);
    for (size_t i=0;i<count;++i) category_->add(T(all[i].label));
    category_->value(0); category_->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->category_changed(); },this);
    field_label(x,y+82,w,T("Value or expression"));
    quantity_=new Input(x,y+108,w,38,this); quantity_->textsize(20); quantity_->maximum_size(ExpressionLimit);
    quantity_->when(FL_WHEN_CHANGED); quantity_->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->schedule_conversion(); },this);
    field_label(x,y+168,w/2-6,T("From unit")); field_label(x+w/2+6,y+168,w/2-6,T("To unit"));
    from_=new Fl_Choice(x,y+196,w/2-6,32); to_=new Fl_Choice(x+w/2+6,y+196,w/2-6,32);
    from_->textsize(14); to_->textsize(14);
    auto changed=[](Fl_Widget *,void *p){ static_cast<Window *>(p)->schedule_conversion(); };
    from_->callback(changed,this); to_->callback(changed,this);
    field_label(x,y+248,w,T("Converted value")); converted_=make_output(x,y+276,w,46); converted_->textsize(24);
    button(x,y+340,174,T("Swap units"),[](Fl_Widget *,void *p){ static_cast<Window *>(p)->swap_units(); },this);
    button(x+192,y+340,174,T("Convert"),calculate_cb,this);
    button(x+384,y+340,w-384,T("Copy result"),copy_cb,this);
    field_label(x,y+388,w,T("Equivalent values"));
    equivalents_=new Fl_Hold_Browser(x,y+416,w,176); equivalents_->textsize(15); equivalents_->format_char(0);
    page->resizable(equivalents_); page->end();
}

void Window::build_statistics() {
    const int x=page_x_,y=page_y_,w=page_w_;
    auto *page=pages_[4]=new Fl_Group(x,y,w,page_h_);
    auto *hint=new Fl_Box(x,y+4,w,46,T("Enter up to 1024 numbers separated by spaces, commas, semicolons or new lines."));
    hint->align(FL_ALIGN_LEFT|FL_ALIGN_WRAP|FL_ALIGN_INSIDE); hint->labelsize(14);
    data_=new Input(x,y+60,w,160,this,FL_MULTILINE_INPUT); data_->maximum_size(32768); data_->textsize(17);
    data_->when(FL_WHEN_CHANGED); data_->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->edited(4); },this);
    button(x,y+238,174,T("Calculate"),calculate_cb,this);
    button(x+192,y+238,174,T("Insert example"),[](Fl_Widget *,void *p){ static_cast<Window *>(p)->example_statistics(); },this);
    button(x+384,y+238,w-384,T("Clear"),clear_cb,this);
    const char *names[]={T("Count"),T("Sum"),T("Mean"),T("Median"),T("Minimum"),T("Maximum"),
                         T("Population variance"),T("Population std. deviation"),T("Sample variance"),T("Sample std. deviation")};
    for (int i=0;i<10;++i) {
        int col=i%2,row=i/2,px=x+col*(w/2+6);
        field_label(px,y+294+row*55,w/2-6,names[i]);
        statistics_[i]=make_output(px,y+317+row*55,w/2-6,28);
    }
    field_label(x,y+576,w,T("Sample variance and deviation require at least two values."));
    page->resizable(data_); page->end();
}

void Window::build_dates() {
    const int x=page_x_,y=page_y_,w=page_w_;
    auto *page=pages_[5]=new Fl_Group(x,y,w,page_h_);
    field_label(x,y+4,w/2-6,T("Start date (YYYY-MM-DD)"));
    field_label(x+w/2+6,y+4,w/2-6,T("End date (YYYY-MM-DD)"));
    for (int i=0;i<2;++i) {
        int px=x+i*(w/2+6);
        dates_[i]=new Input(px,y+34,w/2-6,34,this); dates_[i]->maximum_size(10); dates_[i]->textsize(19);
        dates_[i]->when(FL_WHEN_CHANGED); dates_[i]->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->edited(5); },this);
        button(px,y+80,w/2-6,T("Today"),i==0 ? [](Fl_Widget *,void *p){ static_cast<Window *>(p)->date_today(0); }
               : [](Fl_Widget *,void *p){ static_cast<Window *>(p)->date_today(1); },this);
    }
    button(x,y+134,w,T("Compare dates"),calculate_cb,this);
    field_label(x,y+188,w,T("Calendar days (end minus start)")); difference_=make_output(x,y+214,w,36);
    field_label(x,y+270,w,T("Weekdays (Monday to Friday)")); weekdays_=make_output(x,y+296,w,36);
    auto *hint=new Fl_Box(x,y+346,w,42,T("The end date is excluded. Weekdays do not account for public holidays."));
    hint->align(FL_ALIGN_LEFT|FL_ALIGN_WRAP|FL_ALIGN_INSIDE); hint->labelsize(13);
    field_label(x,y+410,w,T("Days to add to the start date (negative subtracts)"));
    days_=new Input(x,y+438,w-194,34,this,FL_INT_INPUT); days_->maximum_size(9); days_->value("1"); days_->textsize(19);
    days_->when(FL_WHEN_CHANGED); days_->callback([](Fl_Widget *,void *p){ static_cast<Window *>(p)->edited(5); },this);
    button(x+w-178,y+440,178,T("Add days"),[](Fl_Widget *,void *p){ static_cast<Window *>(p)->add_days(); },this);
    date_result_=make_output(x,y+500,w,40); date_result_->textsize(22);
    date_weekday_=new Fl_Box(x,y+554,w,30); date_weekday_->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
    auto *spacer=new Fl_Box(x,y+390,w,4); page->resizable(spacer); page->end();
}
}
