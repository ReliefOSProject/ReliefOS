#include "motif.h"
#include <Xm/Protocols.h>
#include <sys/wait.h>

XtAppContext fileman_app;
Widget fileman_shell;

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget; (void)data; (void)call;
    XtAppSetExitFlag(fileman_app);
}

static void reap_children(XtPointer data, XtIntervalId *id)
{
    (void)data; (void)id;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    XtAppAddTimeOut(fileman_app, 1000, reap_children, NULL);
}

/* The XmNfontList string resource can only describe core fonts, which have no
 * CJK glyphs; route widgets to a CJK-capable Xft rendition instead. */
static XmFontList app_font_list(Widget shell)
{
    Arg args[3];
    XmRendition rendition;
    XtSetArg(args[0], XmNfontName, "SimSun");
    XtSetArg(args[1], XmNfontType, XmFONT_IS_XFT);
    XtSetArg(args[2], XmNloadModel, XmLOAD_IMMEDIATE);
    rendition = XmRenditionCreate(shell, XmFONTLIST_DEFAULT_TAG, args, 3);
    XmFontList list = XmRenderTableAddRenditions(NULL, &rendition, 1, XmDUPLICATE);
    XmRenditionFree(rendition);
    return list;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    XtSetLanguageProc(NULL, NULL, NULL);
    char *fallback[] = {
        "*background: #eceef4", "*foreground: #22242e",
        "*files.background: white", "*folders.background: white",
        "*address.background: white", "*highlightColor: #3b62a6", NULL
    };
    fileman_shell = XtVaAppInitialize(&fileman_app, "ReliefOSFileManager", NULL, 0,
        &argc, argv, fallback, XtNtitle, T("File Manager"),
        XtNwidth, 900, XtNheight, 520, NULL);
    {
        XmFontList fonts = app_font_list(fileman_shell);
        XtVaSetValues(fileman_shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    refresh_home_path();
    fileman_settings_load();
    fileman_tree_reset();
    file_list.selected = -1;
    file_list.visible_rows = 18;
    const char *initial = argc > 1 ? argv[1] : (home_path[0] ? home_path : "/");
    if (navigate_to_path(initial) < 0) navigate_to_path("/");
    fileman_motif_build();
    fileman_motif_refresh();
    XtRealizeWidget(fileman_shell);
    Atom delete_window = XInternAtom(XtDisplay(fileman_shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(fileman_shell, delete_window, close_window, NULL);
    XtAppAddTimeOut(fileman_app, 1000, reap_children, NULL);
    puts("[fileman.elf] Motif file manager ready");
    fflush(stdout);
    XtAppMainLoop(fileman_app);
    XtDestroyWidget(fileman_shell);
    XtDestroyApplicationContext(fileman_app);
    return 0;
}
