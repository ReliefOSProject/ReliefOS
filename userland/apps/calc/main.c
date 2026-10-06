#include "engine.h"
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <stdio.h>
#include <string.h>
#include <X11/keysym.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/TextF.h>

#define T(s) gettext(s)

static struct calc_state state;
static Widget expression, result;
static XtAppContext app;

static void update_display(void)
{
    XmTextFieldSetString(expression, state.expression[0] ? state.expression : "0");
    XmTextFieldSetInsertionPosition(expression, strlen(state.expression));
    XmString text = XmStringCreateLocalized(state.error ? T("Error") : state.result);
    XtVaSetValues(result, XmNlabelString, text, NULL);
    XmStringFree(text);
}

static void input(const char *token)
{
    calc_input(&state, token);
    update_display();
}

static void button(Widget widget, XtPointer token, XtPointer call)
{
    (void)widget;
    (void)call;
    input((const char *)token);
    XmProcessTraversal(expression, XmTRAVERSE_CURRENT);
}

static void key(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)widget;
    (void)data;
    if (event->type != KeyPress) return;
    char buffer[8] = {0};
    KeySym symbol;
    int count = XLookupString(&event->xkey, buffer, sizeof(buffer) - 1, &symbol, NULL);
    if (symbol == XK_Return || symbol == XK_KP_Enter) input("=");
    else if (symbol == XK_BackSpace || symbol == XK_Delete) input("BS");
    else if (symbol == XK_Escape) input("C");
    else if (count == 1) {
        if (buffer[0] == ';') input("C");
        else if (buffer[0] == '[') input("(");
        else if (buffer[0] == ']') input(")");
        else if (buffer[0] == '\\') input("BS");
        else if (strchr("0123456789+-*/()=", buffer[0])) input(buffer);
        else return;
    } else return;
    *dispatch = False;
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    XtAppSetExitFlag(app);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    XtSetLanguageProc(NULL, NULL, NULL);
    char *fallback[] = {
        "*fontList: fixed", "*background: #eceef4", "*foreground: #22242e",
        "*expression.background: white", "*highlightColor: #3b62a6", NULL
    };
    Widget shell = XtVaAppInitialize(&app, "ReliefOSCalculator", NULL, 0,
                                    &argc, argv, fallback, XtNtitle, T("Calculator"),
                                    XtNwidth, 320, XtNheight, 340, NULL);
    Widget form = XtVaCreateWidget("calculator", xmFormWidgetClass, shell,
        XmNwidth, 320, XmNheight, 340, XmNresizePolicy, XmRESIZE_NONE,
        XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    expression = XtVaCreateManagedWidget("expression", xmTextFieldWidgetClass, form,
        XmNeditable, False, XmNcursorPositionVisible, False,
        XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, XmNheight, 36, NULL);
    result = XtVaCreateManagedWidget("result", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_END, XmNheight, 30,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, expression,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XmString caption = XmStringCreateLocalized(T("Integer calculator"));
    Widget footer = XtVaCreateManagedWidget("caption", xmLabelWidgetClass, form,
        XmNlabelString, caption, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    XmStringFree(caption);
    Widget grid = XtVaCreateManagedWidget("keys", xmFormWidgetClass, form,
        XmNfractionBase, 20,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, result,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, footer,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);
    static const char *labels[] = {
        "(", ")", "C", "BS", "7", "8", "9", "/", "4", "5", "6", "*",
        "1", "2", "3", "-", "0", "=", "+", ""
    };
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        XmString label = XmStringCreateLocalized((char *)labels[i]);
        Widget cell = XtVaCreateManagedWidget(labels[i][0] ? labels[i] : "blank",
            xmPushButtonWidgetClass, grid, XmNlabelString, label,
            XmNrecomputeSize, False, XmNsensitive, labels[i][0] != 0,
            XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, (i % 4) * 5,
            XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, (i % 4 + 1) * 5,
            XmNtopAttachment, XmATTACH_POSITION, XmNtopPosition, (i / 4) * 4,
            XmNbottomAttachment, XmATTACH_POSITION, XmNbottomPosition, (i / 4 + 1) * 4,
            XmNleftOffset, 3, XmNrightOffset, 3, XmNtopOffset, 3, XmNbottomOffset, 3, NULL);
        XmStringFree(label);
        XtAddCallback(cell, XmNactivateCallback, button, (XtPointer)labels[i]);
        XtInsertEventHandler(cell, KeyPressMask, False, key, NULL, XtListHead);
    }
    XtInsertEventHandler(expression, KeyPressMask, False, key, NULL, XtListHead);
    update_display();
    XtManageChild(form);
    XtRealizeWidget(shell);
    Atom delete_window = XInternAtom(XtDisplay(shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    XmProcessTraversal(expression, XmTRAVERSE_CURRENT);
    puts("[calc.elf] Motif calculator ready");
    fflush(stdout);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
