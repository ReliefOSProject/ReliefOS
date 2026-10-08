#include "model.h"
#include <fcntl.h>
#include <libintl.h>
#include <locale.h>
#include <reliefos/layout.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>

#define T(s) gettext(s)

#define MS_TILE 28
#define MS_GAP 2
#define MS_INNER (MS_TILE - MS_GAP)
#define MS_BOARD_W (MSW_COLS * MS_TILE)
#define MS_BOARD_H (MSW_ROWS * MS_TILE)

#define TILE_HIDDEN 0x00c0c0c0u
#define TILE_OPEN 0x00d8d8d8u
#define TILE_MINE 0x00e8b0b0u
#define EDGE_LIGHT 0x00ffffffu
#define EDGE_SHADOW 0x00808080u
#define EDGE_DARK 0x00404040u

static struct msw_game game;
static struct msw_sprite mine_sprite;
static struct msw_sprite flag_sprite;
static unsigned long mine_pixels[MSW_SPRITE_SIZE * MSW_SPRITE_SIZE];
static unsigned long flag_pixels[MSW_SPRITE_SIZE * MSW_SPRITE_SIZE];
static Widget board;
static Widget mines_label;
static Widget status_label;
static XtAppContext app;
static Display *display;
static Window board_window;
static GC gc;
static XFontStruct *font;
static unsigned long color_tile_hidden;
static unsigned long color_tile_open;
static unsigned long color_tile_mine;
static unsigned long color_edge_light;
static unsigned long color_edge_shadow;
static unsigned long color_edge_dark;
static unsigned long color_number[9];

static unsigned long alloc_color(unsigned long rgb)
{
    XColor color;
    color.red = (unsigned short)(((rgb >> 16) & 0xffu) * 0x0101u);
    color.green = (unsigned short)(((rgb >> 8) & 0xffu) * 0x0101u);
    color.blue = (unsigned short)((rgb & 0xffu) * 0x0101u);
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, DefaultColormap(display, DefaultScreen(display)), &color)) {
        return BlackPixel(display, DefaultScreen(display));
    }
    return color.pixel;
}

static uint32_t color_for_number(uint8_t n)
{
    switch (n) {
    case 1: return 0x000000bfu;
    case 2: return 0x00008000u;
    case 3: return 0x00bf0000u;
    case 4: return 0x00000080u;
    case 5: return 0x00800000u;
    case 6: return 0x00008080u;
    case 7: return 0x00000000u;
    default: return 0x00808080u;
    }
}

static void set_label(Widget widget, const char *text)
{
    XmString string = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, string, NULL);
    XmStringFree(string);
}

static int load_sprite_file(const char *path, struct msw_sprite *sprite,
                            unsigned long *pixels)
{
    uint8_t buffer[MSW_SPRITE_BMP_MAX_BYTES];
    struct stat st;
    uint32_t len = 0;
    int fd;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size > sizeof(buffer)) {
        return 0;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    while (len < (uint32_t)st.st_size) {
        long got = read(fd, buffer + len, (uint32_t)st.st_size - len);
        if (got <= 0) {
            close(fd);
            return 0;
        }
        len += (uint32_t)got;
    }
    close(fd);
    if (!msw_sprite_parse(buffer, len, sprite)) {
        return 0;
    }
    for (uint32_t i = 0; i < MSW_SPRITE_SIZE * MSW_SPRITE_SIZE; ++i) {
        uint32_t argb = sprite->pixels[i];
        pixels[i] = (argb >> 24) ? alloc_color(argb & 0x00ffffffu) : (unsigned long)-1;
    }
    return 1;
}

static void draw_sprite(int x, int y, const struct msw_sprite *sprite,
                        const unsigned long *pixels)
{
    for (uint32_t yy = 0; yy < sprite->height; ++yy) {
        for (uint32_t xx = 0; xx < sprite->width; ++xx) {
            unsigned long pixel = pixels[yy * MSW_SPRITE_SIZE + xx];
            if (pixel == (unsigned long)-1) {
                continue;
            }
            XSetForeground(display, gc, pixel);
            XDrawPoint(display, board_window, gc, (int)(x + xx), (int)(y + yy));
        }
    }
}

static void draw_tile(uint32_t gx, uint32_t gy)
{
    int x = (int)(gx * MS_TILE);
    int y = (int)(gy * MS_TILE);
    uint8_t cell = game.cells[gy][gx];
    if (cell & MSW_CELL_REVEALED) {
        unsigned long fill = (cell & MSW_CELL_MINE) ? color_tile_mine : color_tile_open;
        XSetForeground(display, gc, fill);
        XFillRectangle(display, board_window, gc, x, y, MS_INNER, MS_INNER);
        XSetForeground(display, gc, color_edge_shadow);
        XDrawLine(display, board_window, gc, x, y, x + MS_INNER - 1, y);
        XDrawLine(display, board_window, gc, x, y, x, y + MS_INNER - 1);
        XSetForeground(display, gc, color_edge_light);
        XDrawLine(display, board_window, gc, x, y + MS_INNER - 1, x + MS_INNER - 1, y + MS_INNER - 1);
        XDrawLine(display, board_window, gc, x + MS_INNER - 1, y, x + MS_INNER - 1, y + MS_INNER - 1);
        if (cell & MSW_CELL_MINE) {
            draw_sprite(x + (int)(MS_INNER - mine_sprite.width) / 2,
                        y + (int)(MS_INNER - mine_sprite.height) / 2,
                        &mine_sprite, mine_pixels);
        } else if (game.adjacent[gy][gx]) {
            char text[2] = {(char)('0' + game.adjacent[gy][gx]), 0};
            int width = font ? XTextWidth(font, text, 1) : 6;
            XSetForeground(display, gc, color_number[game.adjacent[gy][gx]]);
            XDrawString(display, board_window, gc,
                        x + (int)(MS_INNER - width) / 2,
                        y + (int)(MS_INNER + (font ? font->ascent : 10) - (font ? font->descent : 2)) / 2,
                        text, 1);
        }
    } else {
        XSetForeground(display, gc, color_tile_hidden);
        XFillRectangle(display, board_window, gc, x, y, MS_INNER, MS_INNER);
        XSetForeground(display, gc, color_edge_light);
        XDrawLine(display, board_window, gc, x, y, x + MS_INNER - 1, y);
        XDrawLine(display, board_window, gc, x, y, x, y + MS_INNER - 1);
        XSetForeground(display, gc, color_edge_dark);
        XDrawLine(display, board_window, gc, x, y + MS_INNER - 1, x + MS_INNER - 1, y + MS_INNER - 1);
        XDrawLine(display, board_window, gc, x + MS_INNER - 1, y, x + MS_INNER - 1, y + MS_INNER - 1);
        if (cell & MSW_CELL_FLAGGED) {
            draw_sprite(x + (int)(MS_INNER - flag_sprite.width) / 2,
                        y + (int)(MS_INNER - flag_sprite.height) / 2,
                        &flag_sprite, flag_pixels);
        }
    }
}

static void draw_board(void)
{
    for (uint32_t y = 0; y < MSW_ROWS; ++y) {
        for (uint32_t x = 0; x < MSW_COLS; ++x) {
            draw_tile(x, y);
        }
    }
}

static void update_labels(void)
{
    char value[16];
    char mines[48];
    const char *status = T("Ready");
    msw_mines_left_text(&game, value, sizeof(value));
    snprintf(mines, sizeof(mines), "%s%s", T("Mines: "), value);
    set_label(mines_label, mines);
    if (game.game_over) {
        status = game.won ? T("You won") : T("Boom");
    } else if (game.mines_placed) {
        status = T("Playing");
    }
    set_label(status_label, status);
}

static void refresh(void)
{
    draw_board();
    update_labels();
}

static uint32_t new_seed(void)
{
    struct timespec now = {0};
    uint32_t seed;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    seed = (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec ^ 0xa5c35a1du;
    return seed ? seed : 1u;
}

static void new_game(void)
{
    msw_reset(&game, new_seed());
    refresh();
}

static void board_click(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    int gx;
    int gy;
    (void)widget;
    (void)data;
    (void)dispatch;
    if (event->type != ButtonPress) {
        return;
    }
    gx = event->xbutton.x / MS_TILE;
    gy = event->xbutton.y / MS_TILE;
    if (!msw_in_board(gx, gy)) {
        return;
    }
    if (event->xbutton.button == Button3) {
        msw_toggle_flag(&game, gx, gy);
    } else if (event->xbutton.button == Button1) {
        if (!game.mines_placed) {
            msw_place_mines(&game, gx, gy);
        }
        msw_reveal(&game, gx, gy);
    } else {
        return;
    }
    refresh();
}

static void board_expose(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    draw_board();
}

static void button_new_game(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    new_game();
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    XtAppSetExitFlag(app);
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
        "*highlightColor: #3b62a6", NULL
    };
    Widget shell = XtVaAppInitialize(&app, "ReliefOSMinesweeper", NULL, 0,
                                     &argc, argv, fallback,
                                     XtNtitle, T("Minesweeper"),
                                     XtNwidth, 320, XtNheight, 380, NULL);
    {
        XmFontList fonts = app_font_list(shell);
        XtVaSetValues(shell, XmNlabelFontList, fonts,
                      XmNbuttonFontList, fonts, XmNtextFontList, fonts, NULL);
    }
    Widget form = XtVaCreateWidget("minesweeper", xmFormWidgetClass, shell,
        XmNresizePolicy, XmRESIZE_NONE, XmNmarginWidth, 12, XmNmarginHeight, 12, NULL);
    XmString title_text = XmStringCreateLocalized(T("Minesweeper"));
    Widget title = XtVaCreateManagedWidget("title", xmLabelWidgetClass, form,
        XmNlabelString, title_text,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    XmStringFree(title_text);
    Widget new_game_button = XtVaCreateManagedWidget("newGame", xmPushButtonWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM, NULL);
    XmString button_text = XmStringCreateLocalized(T("New Game"));
    XtVaSetValues(new_game_button, XmNlabelString, button_text, NULL);
    XmStringFree(button_text);
    XtAddCallback(new_game_button, XmNactivateCallback, button_new_game, NULL);
    mines_label = XtVaCreateManagedWidget("mines", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, title,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    status_label = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, title,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, mines_label, NULL);
    board = XtVaCreateManagedWidget("board", xmDrawingAreaWidgetClass, form,
        XmNwidth, MS_BOARD_W, XmNheight, MS_BOARD_H,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, mines_label,
        XmNleftAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(board, XmNexposeCallback, board_expose, NULL);
    XtAddEventHandler(board, ButtonPressMask, False, board_click, NULL);
    XtManageChild(form);
    XtRealizeWidget(shell);

    display = XtDisplay(shell);
    board_window = XtWindow(board);
    gc = XCreateGC(display, board_window, 0, NULL);
    font = XLoadQueryFont(display, "fixed");
    if (font) {
        XSetFont(display, gc, font->fid);
    }
    color_tile_hidden = alloc_color(TILE_HIDDEN);
    color_tile_open = alloc_color(TILE_OPEN);
    color_tile_mine = alloc_color(TILE_MINE);
    color_edge_light = alloc_color(EDGE_LIGHT);
    color_edge_shadow = alloc_color(EDGE_SHADOW);
    color_edge_dark = alloc_color(EDGE_DARK);
    for (uint32_t n = 0; n < 9; ++n) {
        color_number[n] = alloc_color(color_for_number((uint8_t)n));
    }
    if (!load_sprite_file(RELIEFOS_PATH_MINESWEEPER_MINE_BMP, &mine_sprite, mine_pixels) ||
        !load_sprite_file(RELIEFOS_PATH_MINESWEEPER_FLAG_BMP, &flag_sprite, flag_pixels)) {
        puts("[minesweeper.elf] required BMP assets unavailable");
        return 1;
    }
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    new_game();
    puts("[minesweeper.elf] Motif minesweeper ready");
    fflush(stdout);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
