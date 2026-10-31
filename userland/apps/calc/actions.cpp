#include "window.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Output.H>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace calc {
namespace {
const unsigned Radices[]={10,16,8,2}, Widths[]={8,16,32,64};
void add_unit_label(Fl_Choice *choice,const char *label) {
    // Fl_Menu_::add treats '/' as a submenu separator unless it is escaped.
    char escaped[512]; size_t n=0;
    for (const char *p=label; *p && n+2<sizeof(escaped); ++p) {
        if (*p=='/' || *p=='\\') escaped[n++]='\\';
        escaped[n++]=*p;
    }
    escaped[n]=0; choice->add(escaped);
}
const char *error_text(Error error) {
    switch (error) {
    case Error::Ok: return T("Ready");
    case Error::Empty: return T("Enter a value or expression.");
    case Error::Syntax: return T("Invalid expression. Check operators and parentheses.");
    case Error::TooLong: return T("Input or nesting limit exceeded.");
    case Error::DivideByZero: return T("Cannot divide by zero.");
    case Error::Domain: return T("The value is outside the supported mathematical or physical domain.");
    case Error::Overflow: return T("The number exceeds the supported range.");
    case Error::InvalidDigit: return T("Invalid digit for the selected number base.");
    case Error::ShiftRange: return T("Shift and rotation counts must be smaller than the word width.");
    case Error::InvalidDate: return T("Use a valid date from 0001-01-01 to 9999-12-31.");
    case Error::TooManyValues: return T("Statistics accepts at most 1024 values.");
    case Error::NotEnoughValues: return T("At least two values are required.");
    }
    return T("Error");
}
const char *day_name(int day) {
    const char *names[]={T("Monday"),T("Tuesday"),T("Wednesday"),T("Thursday"),T("Friday"),T("Saturday"),T("Sunday")};
    return day>=0 && day<7 ? names[day] : "";
}
}
void Window::error(Error code,size_t position) {
    char message[512];
    if (code==Error::Syntax || code==Error::InvalidDigit)
        snprintf(message,sizeof(message),T("%s (position %u)"),error_text(code),unsigned(position+1));
    else snprintf(message,sizeof(message),"%s",error_text(code));
    status_->copy_label(message);
    status_->labelcolor(code==Error::Ok || code==Error::Empty ? FL_FOREGROUND_COLOR : FL_RED); status_->redraw();
}
void Window::select_mode(int mode) {
    if (mode<0 || mode>5) return;
    Fl::remove_timeout(conversion_timeout,this);
    for (int i=0;i<6;++i) if (i==mode) pages_[i]->show(); else pages_[i]->hide();
    mode_=mode; navigation_->value(mode+1); error(Error::Ok);
    if (mode<3) expression_[mode]->take_focus();
    if (mode==3) { quantity_->take_focus(); evaluate_converter(false); }
    if (mode==4) data_->take_focus();
    if (mode==5) dates_[0]->take_focus();
    redraw(); printf("[calc.elf] mode=%d\n",mode); fflush(stdout);
}
int Window::keyboard(int event) {
    if (event==FL_KEYDOWN || event==FL_SHORTCUT) {
        int key=Fl::event_key(),modifiers=Fl::event_state();
        if ((modifiers&FL_CTRL) && key>='1' && key<='6') { select_mode(key-'1'); return 1; }
        if ((modifiers&FL_CTRL) && (modifiers&FL_SHIFT) && (key=='c' || key=='C')) { copy(); return 1; }
        if ((modifiers&FL_CTRL) && (key=='l' || key=='L')) {
            if (mode_<3) expression_[mode_]->take_focus(); else if (mode_==3) quantity_->take_focus(); return 1;
        }
        if (key==FL_F+1) { help(); return 1; }
        if (key==FL_Escape) { clear(); return 1; }
        if (mode_<3 && !(modifiers&(FL_CTRL|FL_ALT)) && Fl::event_length()==1 && Fl::event_text()[0]=='=') {
            calculate(); return 1;
        }
        if (event==FL_KEYDOWN && mode_<3 && evaluated_[mode_] && Fl::focus()==expression_[mode_] &&
            !(modifiers&(FL_CTRL|FL_ALT)) && Fl::event_length()==1) {
            char c=Fl::event_text()[0];
            bool digit=(c>='0' && c<='9') || c=='.' || (mode_<2 && ((c>='a' && c<='z') || (c>='A' && c<='Z'))) || (mode_==2 && Radices[base_->value()]==16 &&
                       ((c>='a' && c<='f') || (c>='A' && c<='F')));
            if (digit || strchr("+-*/^%&|",c)) {
                expression_[mode_]->value(digit ? "" : results_[mode_]);
                expression_[mode_]->position(int(strlen(expression_[mode_]->value()))); edited(mode_);
            }
        }
        if ((key==FL_Enter || key==FL_KP_Enter) && !(modifiers&FL_ALT)) {
            if (mode_==4 && Fl::focus()==data_ && !(modifiers&FL_CTRL)) return 0;
            if (mode_==5 && Fl::focus()==days_) add_days(); else calculate();
            return 1;
        }
    }
    return 0;
}
int Window::handle(int event) { return keyboard(event) ? 1 : Fl_Double_Window::handle(event); }
void Window::edited(int mode) {
    if (mode<0 || mode>5) return;
    results_[mode][0]=0; error(Error::Ok);
    if (mode<3) evaluated_[mode]=false;
    if (mode<2) answer_[mode]->value("");
    if (mode==2) {
        integer_valid_=false; integer_value_=0;
        for (auto *field : bases_) field->value("");
        for (auto *bit : bits_) { bit->value(0); bit->label("0"); bit->redraw(); }
    }
    if (mode==4) for (auto *field : statistics_) field->value("");
    if (mode==5) { difference_->value(""); weekdays_->value(""); date_result_->value(""); date_weekday_->label(""); }
}
void Window::key(const char *token) {
    if (mode_>2) return;
    if (!strcmp(token,"=")) { calculate(); return; }
    if (mode_<2 && !strcmp(token,"C")) { clear(); return; }
    if (mode_<2 && token[0]=='M') { memory(token); return; }
    Fl_Input *field=expression_[mode_];
    if (!strcmp(token,"BS")) {
        int a=field->position(),b=field->mark(); if (a>b) { int t=a; a=b; b=t; }
        if (a==b && a>0) --a;
        field->replace(a,b,""); edited(mode_); field->take_focus(); return;
    }
    char wrapped[ExpressionLimit+1];
    if (!strcmp(token,"negate") || !strcmp(token,"reciprocal") || !strcmp(token,"bitnot")) {
        const char *format=!strcmp(token,"negate") ? "-(%s)" : !strcmp(token,"bitnot") ? "~(%s)" : "1/(%s)";
        int n=snprintf(wrapped,sizeof(wrapped),format,field->value()[0] ? field->value() : "0");
        if (n<0 || size_t(n)>ExpressionLimit) { error(Error::TooLong); return; }
        field->value(wrapped); field->position(int(strlen(wrapped))); edited(mode_); field->take_focus(); return;
    }
    if (evaluated_[mode_]) {
        bool number=(token[0]>='0' && token[0]<='9') || token[0]=='.' || (token[0]>='A' && token[0]<='F');
        bool fresh=number || !strcmp(token,"pi") || !strcmp(token,"e") || !strcmp(token,"ans") || strchr(token,'(');
        field->value(fresh ? "" : results_[mode_]); field->position(int(strlen(field->value())));
    }
    int a=field->position(),b=field->mark(); if (a>b) { int t=a; a=b; b=t; }
    if (strlen(field->value())-size_t(b-a)+strlen(token)>ExpressionLimit) { error(Error::TooLong); return; }
    field->replace(a,b,token); field->position(a+int(strlen(token))); edited(mode_); field->take_focus();
}
void Window::calculate(bool add_history) {
    if (mode_<2) evaluate_real(add_history);
    else if (mode_==2) evaluate_programmer(add_history);
    else if (mode_==3) evaluate_converter(add_history);
    else if (mode_==4) evaluate_statistics(add_history);
    else evaluate_dates(add_history);
}
void Window::evaluate_real(bool add_history) {
    Number value=evaluate(expression_[mode_]->value(),{angles_->value()==0,answer_value_,memory_});
    if (!value.ok()) { answer_[mode_]->value(""); results_[mode_][0]=0; error(value.error,value.position); return; }
    answer_value_=value.value; format_number(value.value,results_[mode_],sizeof(results_[mode_]));
    answer_[mode_]->value(results_[mode_]); evaluated_[mode_]=true; error(Error::Ok);
    if (add_history) record(expression_[mode_]->value(),results_[mode_]);
}
void Window::memory(const char *operation) {
    if (!strcmp(operation,"MC")) memory_=0;
    else if (!strcmp(operation,"MR")) {
        char text[96],token[100]; format_number(memory_,text,sizeof(text)); snprintf(token,sizeof(token),"(%s)",text); key(token); return;
    } else {
        Number value=expression_[mode_]->value()[0] ? evaluate(expression_[mode_]->value(),{angles_->value()==0,answer_value_,memory_})
                                                  : Number{answer_value_,Error::Ok,0};
        if (!value.ok()) { error(value.error,value.position); return; }
        double next=!strcmp(operation,"MS") ? value.value : !strcmp(operation,"M+") ? memory_+value.value : memory_-value.value;
        if (!isfinite(next)) { error(Error::Overflow); return; }
        memory_=next;
    }
    char text[96]; format_number(memory_,text,sizeof(text)); memory_display_->value(text); error(Error::Ok);
}
void Window::show_integer(uint64_t value,bool wrapped) {
    unsigned bits=Widths[width_->value()]; value &= word_mask(bits); integer_value_=value; integer_valid_=true;
    const unsigned bases[]={16,10,8,2};
    for (unsigned i=0;i<4;++i) {
        char text[100]; format_integer(value,bases[i],bits,signed_->value()!=0,text,sizeof(text),i==3);
        if (i==3 && bits==64) text[39]='\n';
        bases_[i]->value(text);
    }
    format_integer(value,Radices[base_->value()],bits,signed_->value()!=0,results_[2],sizeof(results_[2]));
    for (unsigned i=0;i<64;++i) {
        bool set=(value&(UINT64_C(1)<<i))!=0; bits_[i]->value(set); bits_[i]->label(set ? "1" : "0");
        if (i<bits) bits_[i]->activate(); else bits_[i]->deactivate();
        bits_[i]->redraw();
    }
    error(Error::Ok);
    if (wrapped) { status_->copy_label(T("The result was truncated to the selected word width (modular arithmetic).")); status_->redraw(); }
}
void Window::evaluate_programmer(bool add_history) {
    auto value=evaluate_integer(expression_[2]->value(),{Radices[base_->value()],Widths[width_->value()],signed_->value()!=0});
    if (!value.ok()) { edited(2); error(value.error,value.position); return; }
    show_integer(value.value,value.wrapped); evaluated_[2]=true;
    if (add_history) record(expression_[2]->value(),results_[2]);
}
void Window::programmer_options() {
    unsigned base=Radices[base_->value()];
    for (unsigned i=0;i<16;++i) if (digits_[i]) { if (i<base) digits_[i]->activate(); else digits_[i]->deactivate(); }
    if (integer_valid_) { show_integer(integer_value_,false); expression_[2]->value(results_[2]); expression_[2]->position(int(strlen(results_[2]))); evaluated_[2]=true; }
    else if (expression_[2]->value()[0]) evaluate_programmer(false);
    else { show_integer(0,false); integer_valid_=false; results_[2][0]=0; }
}
void Window::bit_changed(unsigned bit) {
    if (bit>=Widths[width_->value()]) return;
    show_integer(integer_value_^(UINT64_C(1)<<bit),false);
    expression_[2]->value(results_[2]); expression_[2]->position(int(strlen(results_[2]))); evaluated_[2]=true;
}
void Window::category_changed() {
    Fl::remove_timeout(conversion_timeout,this);
    size_t count=0; auto *all=categories(&count); int n=category_->value(); if (n<0 || size_t(n)>=count) return;
    const Category &category=all[n]; from_->clear(); to_->clear();
    for (size_t i=0;i<category.count;++i) { add_unit_label(from_,T(category.units[i].label)); add_unit_label(to_,T(category.units[i].label)); }
    from_->value(0); to_->value(category.count>1 ? 1 : 0); if (mode_==3) evaluate_converter(false);
}
void Window::conversion_timeout(void *p) { auto *self=static_cast<Window *>(p); if (self->mode_==3) self->evaluate_converter(false); }
void Window::schedule_conversion() {
    results_[3][0]=0; converted_->value(""); equivalents_->clear();
    Fl::remove_timeout(conversion_timeout,this); Fl::add_timeout(.18,conversion_timeout,this);
}
void Window::swap_units() { int first=from_->value(); from_->value(to_->value()); to_->value(first); evaluate_converter(false); }
void Window::evaluate_converter(bool add_history) {
    Fl::remove_timeout(conversion_timeout,this); results_[3][0]=0; converted_->value(""); equivalents_->clear();
    Number input=evaluate(quantity_->value(),{angles_->value()==0,answer_value_,memory_});
    if (!input.ok()) { if (mode_==3) error(input.error,input.position); return; }
    size_t count=0; auto *all=categories(&count); int n=category_->value();
    if (n<0 || size_t(n)>=count || from_->value()<0 || to_->value()<0) { error(Error::Domain); return; }
    const Category &category=all[n]; Number value=convert(category,size_t(from_->value()),size_t(to_->value()),input.value);
    if (!value.ok()) { error(value.error); return; }
    char result[128],original[128],summary[256]; format_number(value.value,result,sizeof(result)); converted_->value(result);
    snprintf(results_[3],sizeof(results_[3]),"%s %s",result,T(category.units[to_->value()].label));
    for (size_t i=0;i<category.count;++i) {
        auto other=convert(category,size_t(from_->value()),i,input.value); if (!other.ok()) continue;
        format_number(other.value,result,sizeof(result)); snprintf(summary,sizeof(summary),"%s  %s",result,T(category.units[i].label)); equivalents_->add(summary);
    }
    error(Error::Ok);
    if (add_history) { format_number(input.value,original,sizeof(original)); snprintf(summary,sizeof(summary),"%s %s",original,T(category.units[from_->value()].label)); record(summary,results_[3]); }
}
void Window::evaluate_statistics(bool add_history) {
    auto value=statistics(data_->value()); if (!value.ok()) { edited(4); error(value.error,value.position); return; }
    const double values[]={double(value.count),value.sum,value.mean,value.median,value.minimum,value.maximum,
        value.population_variance,value.population_deviation,value.sample_variance,value.sample_deviation};
    const char *names[]={T("Count"),T("Sum"),T("Mean"),T("Median"),T("Minimum"),T("Maximum"),
        T("Population variance"),T("Population std. deviation"),T("Sample variance"),T("Sample std. deviation")};
    size_t used=0; results_[4][0]=0;
    for (unsigned i=0;i<10;++i) {
        char text[96]; format_number(values[i],text,sizeof(text));
        if (i>=8 && value.count<2) snprintf(text,sizeof(text),"%s",T("Not available"));
        statistics_[i]->value(text);
        int n=snprintf(results_[4]+used,sizeof(results_[4])-used,"%s: %s\n",names[i],text);
        if (n>0 && size_t(n)<sizeof(results_[4])-used) used+=size_t(n);
    }
    error(Error::Ok);
    if (add_history) { char text[96]; snprintf(text,sizeof(text),T("%u values"),unsigned(value.count)); record(T("Statistics"),text); }
}
void Window::example_statistics() { data_->value("12, 15, 18, 21, 24\n27, 30, 33, 36, 39"); evaluate_statistics(true); }
void Window::date_today(int field) {
    if (field<0 || field>1) return;
    time_t now=time(nullptr); struct tm local{}; Date date{1970,1,1};
    if (localtime_r(&now,&local)) date={local.tm_year+1900,local.tm_mon+1,local.tm_mday};
    char text[32]; format_date(date,text,sizeof(text)); dates_[field]->value(text); if (mode_==5) edited(5);
}
void Window::evaluate_dates(bool add_history) {
    Date first,second; int days,working;
    if (!parse_date(dates_[0]->value(),&first) || !parse_date(dates_[1]->value(),&second) || !date_difference(first,second,&days,&working)) {
        edited(5); error(Error::InvalidDate); return;
    }
    char text[96]; snprintf(text,sizeof(text),"%d",days); difference_->value(text); snprintf(text,sizeof(text),"%d",working); weekdays_->value(text);
    snprintf(results_[5],sizeof(results_[5]),"%s -> %s: %d %s, %d %s",dates_[0]->value(),dates_[1]->value(),days,T("calendar days"),working,T("weekdays"));
    error(Error::Ok); if (add_history) record(T("Date difference"),results_[5]);
}
void Window::add_days() {
    Date first,result; char *end=nullptr; errno=0; long offset=strtol(days_->value(),&end,10);
    if (!parse_date(dates_[0]->value(),&first) || !days_->value()[0] || *end || errno || offset<INT_MIN || offset>INT_MAX || !date_add(first,int(offset),&result)) {
        edited(5); error(Error::InvalidDate); return;
    }
    char text[32]; format_date(result,text,sizeof(text)); date_result_->value(text); date_weekday_->copy_label(day_name(weekday(result)));
    snprintf(results_[5],sizeof(results_[5]),"%s (%s)",text,day_name(weekday(result))); error(Error::Ok);
    char expression[96]; snprintf(expression,sizeof(expression),"%s + %ld %s",dates_[0]->value(),offset,T("calendar days")); record(expression,results_[5]);
}
void Window::record(const char *expression,const char *result) {
    unsigned kept=history_count_<64 ? history_count_ : 63; if (kept) memmove(entries_+1,entries_,kept*sizeof(entries_[0]));
    if (history_count_<64) ++history_count_;
    entries_[0].mode=mode_; snprintf(entries_[0].expression,sizeof(entries_[0].expression),"%s",expression);
    snprintf(entries_[0].result,sizeof(entries_[0].result),"%s",result);
    char line[1400]; snprintf(line,sizeof(line),"%s = %s",expression,result); history_->insert(1,line);
    while (history_->size()>64) history_->remove(history_->size());
    printf("[calc.elf] evaluated mode=%d\n",mode_); fflush(stdout);
}
void Window::history_selected() {
    int index=history_->value()-1; if (index<0 || unsigned(index)>=history_count_ || !Fl::event_clicks()) return;
    const History &entry=entries_[index];
    if (entry.mode<2) { select_mode(entry.mode); expression_[mode_]->value(entry.expression); edited(mode_); calculate(false); }
    else { Fl::copy(entry.result,int(strlen(entry.result)),1); error(Error::Ok); }
}
void Window::clear_history() { history_count_=0; memset(entries_,0,sizeof(entries_)); history_->clear(); }
void Window::copy() {
    if (!results_[mode_][0]) return;
    Fl::copy(results_[mode_],int(strlen(results_[mode_])),1); status_->copy_label(T("Result copied to clipboard."));
    status_->labelcolor(FL_FOREGROUND_COLOR); status_->redraw();
}
void Window::clear() {
    if (mode_<3) {
        expression_[mode_]->value(""); edited(mode_);
        if (mode_<2) answer_[mode_]->value("0"); else { show_integer(0,false); integer_valid_=false; results_[2][0]=0; }
        expression_[mode_]->take_focus();
    } else if (mode_==3) { quantity_->value(""); Fl::remove_timeout(conversion_timeout,this); results_[3][0]=0; converted_->value(""); equivalents_->clear(); }
    else if (mode_==4) { data_->value(""); edited(4); }
    else { dates_[0]->value(""); dates_[1]->value(""); days_->value("1"); edited(5); }
    error(Error::Ok);
}
void Window::menu(int action) { if (action==0) hide(); else help(); }
void Window::help() {
    Fl_Double_Window dialog(760,560,T("Calculator help")); Fl_Help_View view(12,12,736,492); view.textsize(15);
    view.value(T("<p><b>Standard and scientific</b><br>Use + - * / ^, parentheses, postfix % and !. 200*10% = 20. ans is the last answer, mem is memory. Powers associate right to left; -2^2 = -4. Use explicit multiplication: 2*pi.</p><p>Functions: sin, cos, tan, asin, acos, atan, sinh, cosh, tanh, sqrt, cbrt, ln, log, exp, abs, floor, ceil, round; pow(a,b), min(a,b), max(a,b), hypot(a,b), mod(a,b), atan2(y,x). Choose degrees or radians.</p><p><b>Programmer</b><br>Integer DEC/HEX/OCT/BIN, 8/16/32/64 bits. Prefixes: 0d, 0x, 0o, 0b. Operators: + - * / %, AND OR XOR NOT, &amp; | ^ ~, &lt;&lt; &gt;&gt;; rol(value,count), ror(value,count). Arithmetic wraps to the word width. Signed mode uses two's complement, signed division and arithmetic right shift. Shift counts must be smaller than the width. Calculated values keep their bit pattern when the base changes.</p><p><b>Conversions</b><br>Choose units and enter a value or expression. Preview is automatic; Convert adds history. Temperatures below absolute zero are rejected. MB is decimal, MiB is binary. US and UK volumes are distinct.</p><p><b>Statistics and dates</b><br>Up to 1024 finite numbers; sample variance requires two. Dates use the Gregorian calendar (0001 to 9999). Differences exclude the end date. Weekdays exclude Saturday and Sunday, without holidays. Add days uses whole calendar days.</p><p>Ctrl+1..6 switches mode. Enter calculates; Ctrl+Enter calculates statistics. Esc clears input. Ctrl+Shift+C copies the result. Double-click history to reuse an expression (other modes copy the result). Memory and history last for this session.</p>"));
    Fl_Button close(640,518,108,30,T("Close")); close.callback([](Fl_Widget *,void *p){ static_cast<Fl_Window *>(p)->hide(); },&dialog);
    dialog.end(); reliefos_fltk::style(&dialog); dialog.resizable(view); dialog.set_modal(); dialog.show(); while (dialog.shown()) Fl::wait();
}
}
