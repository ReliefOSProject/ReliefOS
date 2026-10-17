#include "../fltk_support.h"
#include "../fltk_files.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <stdio.h>
#include <time.h>
extern "C" {
#include "model.h"
}

static msw_game game;
static msw_sprite mine_sprite, flag_sprite;
static Fl_Box *mines_label, *status_label;
static Fl_Widget *board;
static const int tile = 28;

static bool load_sprite(const char *path, msw_sprite *sprite)
{
    unsigned char *data = nullptr;
    unsigned count = 0;
    if (reliefos_fltk::read_file(path, MSW_SPRITE_BMP_MAX_BYTES, &data, &count) < 0)
        return false;
    bool valid = msw_sprite_parse(data, count, sprite);
    free(data);
    return valid;
}

static void refresh()
{
    char value[16], text[64];
    msw_mines_left_text(&game, value, sizeof(value));
    snprintf(text, sizeof(text), "%s%s", T("Mines: "), value);
    mines_label->copy_label(text);
    status_label->copy_label(game.game_over ? (game.won ? T("You won") : T("Boom")) :
        (game.mines_placed ? T("Playing") : T("Ready")));
    board->redraw();
}

static void new_game(Fl_Widget *, void *)
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    msw_reset(&game, static_cast<uint32_t>(now.tv_nsec ^ now.tv_sec ^ 0xa5c35a1d));
    refresh();
}

class Board : public Fl_Widget {
    void sprite(int x, int y, const msw_sprite &image)
    {
        for (unsigned yy = 0; yy < image.height; ++yy)
            for (unsigned xx = 0; xx < image.width; ++xx) {
                uint32_t argb = image.pixels[yy * MSW_SPRITE_SIZE + xx];
                if (argb >> 24) { fl_color(fl_rgb_color(argb >> 16, argb >> 8, argb)); fl_point(x + xx, y + yy); }
            }
    }
public:
    Board(int x, int y) : Fl_Widget(x, y, MSW_COLS * tile, MSW_ROWS * tile) {}
    void draw() override
    {
        static const unsigned colors[] = {0, 0x0000bf, 0x008000, 0xbf0000, 0x000080, 0x800000, 0x008080, 0, 0x808080};
        fl_push_clip(x(), y(), w(), h());
        fl_color(FL_BACKGROUND_COLOR); fl_rectf(x(), y(), w(), h());
        for (int gy = 0; gy < MSW_ROWS; ++gy) for (int gx = 0; gx < MSW_COLS; ++gx) {
            unsigned cell = game.cells[gy][gx];
            int px = x() + gx * tile, py = y() + gy * tile;
            bool open = (cell & MSW_CELL_REVEALED) != 0;
            fl_draw_box(open ? FL_DOWN_BOX : FL_UP_BOX, px, py, tile-2, tile-2,
                open && (cell & MSW_CELL_MINE) ? fl_rgb_color(232,176,176) : FL_LIGHT2);
            if (open && (cell & MSW_CELL_MINE)) sprite(px + 3, py + 3, mine_sprite);
            else if (!open && (cell & MSW_CELL_FLAGGED)) sprite(px + 3, py + 3, flag_sprite);
            else if (open && game.adjacent[gy][gx]) {
                unsigned count = game.adjacent[gy][gx], rgb = colors[count];
                char number[] = {static_cast<char>('0' + count), 0};
                fl_font(FL_HELVETICA_BOLD, 18); fl_color(fl_rgb_color(rgb >> 16, rgb >> 8, rgb));
                fl_draw(number, px, py, tile-2, tile-2, FL_ALIGN_CENTER);
            }
        }
        fl_pop_clip();
    }
    int handle(int event) override
    {
        if (event != FL_PUSH) return Fl_Widget::handle(event);
        int gx = (Fl::event_x() - x()) / tile, gy = (Fl::event_y() - y()) / tile;
        if (!msw_in_board(gx, gy)) return 0;
        if (Fl::event_button() == FL_RIGHT_MOUSE) msw_toggle_flag(&game, gx, gy);
        else if (Fl::event_button() == FL_LEFT_MOUSE) {
            if (!game.mines_placed) msw_place_mines(&game, gx, gy);
            msw_reveal(&game, gx, gy);
        } else return 0;
        refresh();
        return 1;
    }
};

int main()
{
    reliefos_fltk::init();
    if (!load_sprite(RELIEFOS_PATH_MINESWEEPER_MINE_BMP, &mine_sprite) ||
        !load_sprite(RELIEFOS_PATH_MINESWEEPER_FLAG_BMP, &flag_sprite)) {
        fprintf(stderr, "[minesweeper.elf] required BMP assets unavailable\n"); return 1;
    }
    Fl_Double_Window window(340, 390, T("Minesweeper"));
    Fl_Button *restart = new Fl_Button(168, 12, 154, 32, T("New game"));
    restart->callback(new_game);
    mines_label = new Fl_Box(16, 12, 148, 32);
    board = new Board(44, 64);
    status_label = new Fl_Box(16, 338, 308, 30);
    window.end(); reliefos_fltk::style(&window); new_game(nullptr, nullptr); window.show();
    puts("[minesweeper.elf] FLTK minesweeper ready");
    return Fl::run();
}
