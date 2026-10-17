#include "../fltk_support.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Box.H>
#include <stdio.h>
#include <string.h>
extern "C" {
#include "engine.h"
}

static calc_state state;
static Fl_Output *expression;
static Fl_Box *result;

static void input(const char *token)
{
    calc_input(&state, token);
    expression->value(state.expression[0] ? state.expression : "0");
    result->copy_label(state.error ? T("Error") : state.result);
    result->redraw();
}

static int calculator_key_event(int event)
{
    if (event != FL_KEYDOWN && event != FL_SHORTCUT) return 0;
    int key = Fl::event_key();
    if (key == FL_Enter || key == FL_KP_Enter) { input("="); return 1; }
    if (key == FL_BackSpace || key == FL_Delete) { input("BS"); return 1; }
    if (key == FL_Escape) { input("C"); return 1; }
    const char *text = Fl::event_text();
    if (Fl::event_length() == 1) {
        switch (text[0]) {
        case ';': input("C"); return 1;
        case '[': input("("); return 1;
        case ']': input(")"); return 1;
        case '\\': input("BS"); return 1;
        default: break;
        }
        if (strchr("0123456789+-*/()=", text[0])) {
            char token[] = {text[0], 0};
            input(token);
            return 1;
        }
    }
    return 0;
}

class Expression : public Fl_Output {
public:
    Expression(int x, int y, int w, int h) : Fl_Output(x, y, w, h) {}
    int handle(int event) override
    {
        // Fl_Output consumes text keys even though it is read-only. Route
        // calculator input before that handler; keep selection/copy behavior.
        return calculator_key_event(event) ? 1 : Fl_Output::handle(event);
    }
};

class Calculator : public Fl_Double_Window {
public:
    Calculator() : Fl_Double_Window(340, 390, T("Calculator")) {}
    int handle(int event) override
    {
        return calculator_key_event(event) ? 1 : Fl_Double_Window::handle(event);
    }
};

static void press(Fl_Widget *, void *token) { input(static_cast<const char *>(token)); }

int main()
{
    reliefos_fltk::init();
    Calculator window;
    expression = new Expression(14, 14, 312, 38);
    expression->textsize(20);
    expression->value("0");
    result = new Fl_Box(14, 58, 312, 32, "0");
    result->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    static const char *keys[] = {
        "(", ")", "C", "BS", "7", "8", "9", "/", "4", "5", "6", "*",
        "1", "2", "3", "-", "0", "=", "+", ""
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (!keys[i][0]) continue;
        Fl_Button *button = new Fl_Button(14 + (i % 4) * 80, 100 + (i / 4) * 48, 72, 40, keys[i]);
        button->callback(press, const_cast<char *>(keys[i]));
        button->clear_visible_focus();
    }
    new Fl_Box(14, 348, 312, 28, T("Integer calculator"));
    window.end();
    reliefos_fltk::style(&window);
    window.show();
    puts("[calc.elf] FLTK calculator ready");
    return Fl::run();
}
