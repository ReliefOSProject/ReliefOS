#ifndef RELIEFOS_CALC_WINDOW_H
#define RELIEFOS_CALC_WINDOW_H
#include "model.h"
#include "../fltk_support.h"
#include <FL/Fl_Double_Window.H>
class Fl_Input;
class Fl_Box;
class Fl_Output;
class Fl_Choice;
class Fl_Check_Button;
class Fl_Hold_Browser;
class Fl_Button;

namespace calc {
class Window : public Fl_Double_Window {
    struct History { int mode; char expression[ExpressionLimit+1]; char result[256]; };
    int mode_ = 0;
    Fl_Group *content_ = nullptr, *pages_[6] = {};
    Fl_Hold_Browser *navigation_ = nullptr, *history_ = nullptr;
    Fl_Input *expression_[3] = {};
    Fl_Output *answer_[2] = {}, *memory_display_ = nullptr;
    Fl_Box *status_ = nullptr;
    Fl_Choice *angles_ = nullptr, *base_ = nullptr, *width_ = nullptr;
    Fl_Check_Button *signed_ = nullptr;
    Fl_Output *bases_[4] = {};
    Fl_Button *bits_[64] = {}, *digits_[16] = {};
    Fl_Choice *category_ = nullptr, *from_ = nullptr, *to_ = nullptr;
    Fl_Input *quantity_ = nullptr;
    Fl_Output *converted_ = nullptr;
    Fl_Hold_Browser *equivalents_ = nullptr;
    Fl_Input *data_ = nullptr;
    Fl_Output *statistics_[10] = {};
    Fl_Input *dates_[2] = {}, *days_ = nullptr;
    Fl_Output *difference_ = nullptr, *weekdays_ = nullptr, *date_result_ = nullptr;
    Fl_Box *date_weekday_ = nullptr;
    double answer_value_ = 0, memory_ = 0;
    uint64_t integer_value_ = 0;
    bool integer_valid_ = false, evaluated_[3] = {};
    History entries_[64] = {};
    unsigned history_count_ = 0;
    char results_[6][2048] = {};
    int page_x_, page_y_, page_w_, page_h_;
    void build_calculator(int mode);
    void build_programmer();
    void build_converter();
    void build_statistics();
    void build_dates();
    Fl_Output *make_output(int x,int y,int w,int h,bool multiline = false);
    void evaluate_real(bool record);
    void evaluate_programmer(bool record);
    void evaluate_converter(bool record);
    void evaluate_statistics(bool record);
    void evaluate_dates(bool record);
    void show_integer(uint64_t value, bool wrapped);
    void error(Error error, size_t position = 0);
    void record(const char *expression, const char *result);
    void memory(const char *operation);
    void help();
    static void conversion_timeout(void *self);
public:
    Window();
    ~Window() override;
    int handle(int event) override;
    int keyboard(int event);
    void select_mode(int mode);
    void key(const char *token);
    void calculate(bool record = true);
    void clear();
    void copy();
    void clear_history();
    void history_selected();
    void edited(int mode);
    void programmer_options();
    void category_changed();
    void schedule_conversion();
    void swap_units();
    void bit_changed(unsigned bit);
    void example_statistics();
    void date_today(int field);
    void add_days();
    void menu(int action);
};
}
#endif
