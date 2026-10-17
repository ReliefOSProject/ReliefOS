#ifndef RELIEFEXCEL_GRID_H
#define RELIEFEXCEL_GRID_H
#include "model.h"
#include <FL/Fl_Table.H>
#include <FL/Fl_Input.H>
namespace rx {
class ExcelWindow;
class CellInput;
class SheetGrid : public Fl_Table {
    struct Slot { int index = 0, position = 0, extent = 0; bool frozen = false; };
    ExcelWindow *owner_;
    CellInput *input_;
    Array<int> visible_rows_;
    Array<Slot> rows_, columns_;
    Range selection_, fill_source_, fill_target_;
    int anchor_row_ = 0, anchor_col_ = 0;
    int cursor_row_ = -1, cursor_col_ = -1;
    bool dragging_ = false, filling_ = false;
    int resizing_ = -1, resize_start_ = 0, resize_pixels_ = 0, resize_original_ = 0;
    bool resize_rows_ = false;
    uint64_t layout_revision_ = 0;
    uint32_t layout_sheet_ = 0;
    int header_width_ = 52, header_height_ = 26;
    void layout_cells();
    void rebuild_visible();
    bool hit(int x, int y, int &row, int &col) const;
    bool rectangle(Range range, int &x, int &y, int &w, int &h, bool frozen_row, bool frozen_col) const;
    void draw_cell(TableContext context, int row, int col, int x, int y, int w, int h) override;
    int column_pixels(int col) const;
    int row_pixels(int row) const;
    void ensure_visible(int row, int col);
    static void scroll_cb(Fl_Widget *, void *);
public:
    SheetGrid(int x, int y, int w, int h, ExcelWindow *owner);
    Range selection() const { return selection_; }
    void select(int row, int col, bool extend = false);
    void select_range(Range range);
    void refresh(bool ensure_cursor = false);
    void begin_edit(const char *initial = nullptr, bool replace = false);
    bool finish_edit(bool commit, int row_delta = 0, int col_delta = 0);
    bool editing() const;
    void move(int row_delta, int col_delta, bool extend = false);
    void draw() override;
    void resize(int,int,int,int) override;
    int handle(int event) override;
};
}
#endif
