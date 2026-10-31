#include "grid.h"
#include "window.h"
#include "formula.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Enumerations.H>
#include <stdio.h>
#include <string.h>
#include <math.h>
namespace rx {
namespace {
int clamp(int n,int low,int high) { return n<low?low:n>high?high:n; }
Fl_Color rgb_color(uint32_t rgb) { return fl_rgb_color(rgb>>16,(rgb>>8)&255,rgb&255); }
const Fl_Color accent = fl_rgb_color(30,111,68);
}
class CellInput : public Fl_Input {
    SheetGrid *grid_;
public:
    CellInput(int x,int y,int w,int h,SheetGrid *grid) : Fl_Input(x,y,w,h),grid_(grid) { maximum_size(MaxTextBytes); }
    int handle(int event) override {
        if (event==FL_KEYDOWN && !Fl::compose_state) {
            int key=Fl::event_key();
            if (key==FL_Escape) { grid_->finish_edit(false); return 1; }
            if (key==FL_Enter || key==FL_KP_Enter) { grid_->finish_edit(true,Fl::event_state(FL_SHIFT)?-1:1,0); return 1; }
            if (key==FL_Tab) { grid_->finish_edit(true,0,Fl::event_state(FL_SHIFT)?-1:1); return 1; }
        }
        return Fl_Input::handle(event);
    }
};
SheetGrid::SheetGrid(int X,int Y,int W,int H,ExcelWindow *owner)
    : Fl_Table(X,Y,W,H),owner_(owner) {
    table->hide();
    begin(); input_=new CellInput(X,Y,100,26,this); input_->hide(); end();
    vscrollbar->callback(scroll_cb,this); hscrollbar->callback(scroll_cb,this);
    vscrollbar->step(1); hscrollbar->step(1);
    selection_={0,0,0,0};
    box(FL_NO_BOX);
}
bool SheetGrid::editing() const { return input_->visible(); }
int SheetGrid::column_pixels(int col) const {
    const Sheet *s=owner_->sheet(); if (!s) return 104;
    int size=col<int(s->column_widths.size()) && s->column_widths[col]>0?s->column_widths[col]:104;
    if (resizing_==col && !resize_rows_) size=resize_pixels_;
    return clamp(size*s->zoom/100,12,2400);
}
int SheetGrid::row_pixels(int row) const {
    const Sheet *s=owner_->sheet(); if (!s) return 26;
    int size=row<int(s->row_heights.size()) && s->row_heights[row]>0?s->row_heights[row]:26;
    if (resizing_==row && resize_rows_) size=resize_pixels_;
    return clamp(size*s->zoom/100,8,2400);
}
void SheetGrid::rebuild_visible() {
    Sheet *s=owner_->sheet();
    if (!s || (layout_revision_==owner_->editor.book.revision && layout_sheet_==s->id)) return;
    visible_rows_.clear();
    if (!visible_rows_.reserve(MaxRows)) { owner_->message(T("Not enough memory")); return; }
    for (int row=0;row<MaxRows;++row) if ((!s->freeze_row || row!=0) && s->row_visible(row)) visible_rows_.push(row);
    layout_revision_=owner_->editor.book.revision; layout_sheet_=s->id;
}
void SheetGrid::layout_cells() {
    Sheet *s=owner_->sheet(); if (!s) return;
    rebuild_visible();
    rows_.clear(); columns_.clear();
    const int bar=Fl::scrollbar_size(), right=x()+w()-bar,bottom=y()+h()-bar;
    header_width_=52; header_height_=26;
    int py=y()+header_height_;
    if (s->freeze_row) { int rh=row_pixels(0); rows_.push(Slot{0,py,rh,true}); py+=rh; }
    int top=clamp(s->scroll_row,0,visible_rows_.size()?int(visible_rows_.size()-1):0);
    s->scroll_row=top;
    int count=0;
    for (int i=top;i<int(visible_rows_.size()) && py<bottom;++i) {
        int row=visible_rows_[i],rh=row_pixels(row);
        rows_.push(Slot{row,py,rh,false}); py+=rh; ++count;
    }
    int px=x()+header_width_,first=clamp(s->scroll_col,s->freeze_column?1:0,MaxCols-1);
    s->scroll_col=first;
    if (s->freeze_column) { int cw=column_pixels(0); columns_.push(Slot{0,px,cw,true}); px+=cw; }
    int cc=0;
    for (int col=first;col<MaxCols && px<right;++col) {
        int cw=column_pixels(col); columns_.push(Slot{col,px,cw,false}); px+=cw; ++cc;
    }
    vscrollbar->resize(right,y()+header_height_,bar,h()-header_height_-bar);
    hscrollbar->resize(x()+header_width_,bottom,w()-header_width_-bar,bar);
    vscrollbar->value(top,count>1?count-1:1,0,int(visible_rows_.size()));
    hscrollbar->value(first,cc>1?cc-1:1,s->freeze_column?1:0,MaxCols);
    vscrollbar->show(); hscrollbar->show();
}
void SheetGrid::scroll_cb(Fl_Widget *,void *context) {
    SheetGrid *g=static_cast<SheetGrid *>(context);
    if (g->owner_->is_busy()) return;
    if (!g->finish_edit(true)) return;
    Sheet *s=g->owner_->sheet(); if (!s) return;
    s->scroll_row=int(g->vscrollbar->value()); s->scroll_col=int(g->hscrollbar->value());
    g->redraw();
}
bool SheetGrid::hit(int px,int py,int &row,int &col) const {
    row=col=-1;
    for (size_t n=0;n<rows_.size();++n) if (py>=rows_[n].position && py<rows_[n].position+rows_[n].extent) { row=rows_[n].index; break; }
    for (size_t n=0;n<columns_.size();++n) if (px>=columns_[n].position && px<columns_[n].position+columns_[n].extent) { col=columns_[n].index; break; }
    if (px<x()+header_width_) col=-1;
    if (py<y()+header_height_) row=-1;
    return row>=0 || col>=0;
}
bool SheetGrid::rectangle(Range range,int &X,int &Y,int &W,int &H,bool frozen_row,bool frozen_col) const {
    bool hasrow=false,hascol=false; int right=0,bottom=0;
    for (size_t n=0;n<rows_.size();++n) {
        const Slot &slot=rows_[n];
        if (slot.frozen!=frozen_row || slot.index<range.r1 || slot.index>range.r2) continue;
        if (!hasrow) { Y=slot.position; hasrow=true; }
        bottom=slot.position+slot.extent;
    }
    for (size_t n=0;n<columns_.size();++n) {
        const Slot &slot=columns_[n];
        if (slot.frozen!=frozen_col || slot.index<range.c1 || slot.index>range.c2) continue;
        if (!hascol) { X=slot.position; hascol=true; }
        right=slot.position+slot.extent;
    }
    W=right-X; H=bottom-Y;
    return hasrow && hascol;
}
void SheetGrid::draw_cell(TableContext context,int row,int col,int X,int Y,int W,int H) {
    Sheet *s=owner_->sheet(); if (!s) return;
    if (context==CONTEXT_ROW_HEADER || context==CONTEXT_COL_HEADER) {
        bool selected=context==CONTEXT_ROW_HEADER?(row>=selection_.r1&&row<=selection_.r2):(col>=selection_.c1&&col<=selection_.c2);
        fl_draw_box(FL_THIN_UP_BOX,X,Y,W,H,selected?fl_rgb_color(221,239,225):FL_BACKGROUND_COLOR);
        char label[32];
        if (context==CONTEXT_ROW_HEADER) snprintf(label,sizeof(label),"%d",row+1);
        else column_label(col,label,sizeof(label));
        fl_color(selected?accent:FL_FOREGROUND_COLOR); fl_font(FL_HELVETICA,13); fl_draw(label,X,Y,W,H,FL_ALIGN_CENTER);
        if (context==CONTEXT_COL_HEADER && s->filter_enabled && col>=s->filter_range.c1 && col<=s->filter_range.c2) {
            fl_color(accent); fl_polygon(X+W-15,Y+H/2-2,X+W-7,Y+H/2-2,X+W-11,Y+H/2+3);
        }
        return;
    }
    const Cell *cell=s->get(row,col); Style style=cell?cell->style:Style{};
    bool selected=selection_.contains(row,col);
    fl_color(selected?fl_color_average(rgb_color(style.background),fl_rgb_color(197,225,205),0.55f):rgb_color(style.background));
    fl_rectf(X,Y,W,H);
    if (s->gridlines) { fl_color(fl_rgb_color(221,226,222)); fl_rect(X,Y,W+1,H+1); }
    fl_push_clip(X+1,Y+1,W>2?W-2:0,H>2?H-2:0);
    if (cell && !cell->input.empty()) {
        Text display;
        if (owner_->show_formulas && !cell->literal && cell->input.c_str()[0]=='=') display=cell->input;
        else display=format_value(owner_->is_busy()?cell->result:evaluate(owner_->editor.book,s->id,row,col),style);
        fl_color(rgb_color(style.foreground));
        fl_font(Fl_Font(FL_HELVETICA+(style.bold?1:0)+(style.italic?2:0)),clamp(style.size*s->zoom/100,6,96));
        bool numeric=cell->result.kind==ValueKind::Number;
        Fl_Align align=FL_ALIGN_INSIDE;
        if (style.alignment==Alignment::Center) align|=FL_ALIGN_CENTER;
        else if (style.alignment==Alignment::Right || (style.alignment==Alignment::Auto && numeric && !owner_->show_formulas)) align|=FL_ALIGN_RIGHT;
        else align|=FL_ALIGN_LEFT;
        if (style.wrap) align|=FL_ALIGN_WRAP;
        fl_draw(display.c_str(),X+5,Y+2,W>10?W-10:0,H>4?H-4:0,align,nullptr,0);
    }
    fl_pop_clip();
    if (style.borders) {
        fl_color(rgb_color(style.foreground));
        if (style.borders&1) fl_line(X,Y,X+W,Y);
        if (style.borders&2) fl_line(X+W,Y,X+W,Y+H);
        if (style.borders&4) fl_line(X,Y+H,X+W,Y+H);
        if (style.borders&8) fl_line(X,Y,X,Y+H);
    }
}
void SheetGrid::draw() {
    layout_cells(); Sheet *s=owner_->sheet(); if (!s) return;
    const int bar=Fl::scrollbar_size();
    fl_push_clip(x(),y(),w(),h()); fl_color(FL_BACKGROUND2_COLOR); fl_rectf(x(),y(),w(),h());
    fl_push_clip(x()+header_width_,y()+header_height_,w()-header_width_-bar,h()-header_height_-bar);
    for (size_t ri=0;ri<rows_.size();++ri) for (size_t ci=0;ci<columns_.size();++ci) {
        const Slot &r=rows_[ri],&c=columns_[ci];
        const Range *merged=nullptr;
        for (size_t m=0;m<s->merges.size();++m) if (s->merges[m].contains(r.index,c.index)) { merged=&s->merges[m]; break; }
        if (!merged) draw_cell(CONTEXT_CELL,r.index,c.index,c.position,r.position,c.extent,r.extent);
        else {
            int X=0,Y=0,W=0,H=0;
            if (!rectangle(*merged,X,Y,W,H,r.frozen,c.frozen) || X!=c.position || Y!=r.position) continue;
            draw_cell(CONTEXT_CELL,merged->r1,merged->c1,X,Y,W,H);
        }
    }
    fl_color(accent); fl_line_style(FL_SOLID,2);
    Range outline=filling_?fill_target_:selection_;
    for (int fr=0;fr<2;++fr) for (int fc=0;fc<2;++fc) {
        int X=0,Y=0,W=0,H=0;
        if (rectangle(outline,X,Y,W,H,fr,fc)) fl_rect(X,Y,W,H);
    }
    for (size_t ri=0;ri<rows_.size();++ri) for (size_t ci=0;ci<columns_.size();++ci)
        if (rows_[ri].index==selection_.r2 && columns_[ci].index==selection_.c2)
            fl_rectf(columns_[ci].position+columns_[ci].extent-3,rows_[ri].position+rows_[ri].extent-3,6,6);
    fl_line_style(0);
    if (s->freeze_row && !rows_.empty()) { fl_color(accent); fl_line(x()+header_width_,rows_[0].position+rows_[0].extent,x()+w()-bar,rows_[0].position+rows_[0].extent); }
    if (s->freeze_column && !columns_.empty()) { fl_color(accent); fl_line(columns_[0].position+columns_[0].extent,y()+header_height_,columns_[0].position+columns_[0].extent,y()+h()-bar); }
    fl_pop_clip();
    for (size_t n=0;n<rows_.size();++n) draw_cell(CONTEXT_ROW_HEADER,rows_[n].index,0,x(),rows_[n].position,header_width_,rows_[n].extent);
    for (size_t n=0;n<columns_.size();++n) draw_cell(CONTEXT_COL_HEADER,0,columns_[n].index,columns_[n].position,y(),columns_[n].extent,header_height_);
    fl_draw_box(FL_THIN_UP_BOX,x(),y(),header_width_,header_height_,FL_BACKGROUND_COLOR);
    fl_pop_clip();
    draw_child(*vscrollbar); draw_child(*hscrollbar);
    if (input_->visible()) draw_child(*input_);
}
void SheetGrid::resize(int X,int Y,int W,int H) {
    Fl_Group::resize(X,Y,W,H); layout_cells(); redraw();
}
void SheetGrid::refresh(bool ensure_cursor) {
    layout_revision_=0;
    Sheet *s=owner_->sheet(); if (!s) return;
    if (ensure_cursor || layout_sheet_!=s->id || cursor_row_!=s->cursor_row || cursor_col_!=s->cursor_col) {
        input_->hide(); selection_={s->cursor_row,s->cursor_col,s->cursor_row,s->cursor_col};
        anchor_row_=s->cursor_row; anchor_col_=s->cursor_col;
    }
    cursor_row_=s->cursor_row; cursor_col_=s->cursor_col;
    layout_cells(); if (ensure_cursor) ensure_visible(s->cursor_row,s->cursor_col); redraw();
}
void SheetGrid::ensure_visible(int row,int col) {
    Sheet *s=owner_->sheet(); if (!s) return;
    layout_cells();
    bool hasrow=s->freeze_row&&row==0,hascol=s->freeze_column&&col==0;
    for (size_t n=0;n<rows_.size();++n) if (rows_[n].index==row && rows_[n].position+rows_[n].extent<=y()+h()-Fl::scrollbar_size()) hasrow=true;
    for (size_t n=0;n<columns_.size();++n) if (columns_[n].index==col && columns_[n].position+columns_[n].extent<=x()+w()-Fl::scrollbar_size()) hascol=true;
    if (!hasrow) for (size_t n=0;n<visible_rows_.size();++n) if (visible_rows_[n]==row) {
        int visible=int(rows_.size())-(s->freeze_row?1:0);
        s->scroll_row=int(n)<s->scroll_row?int(n):clamp(int(n)-visible+2,0,int(visible_rows_.size()-1)); break;
    }
    if (!hascol) {
        int visible=int(columns_.size())-(s->freeze_column?1:0);
        s->scroll_col=col<s->scroll_col?col:clamp(col-visible+2,s->freeze_column?1:0,MaxCols-1);
    }
    layout_cells();
}
void SheetGrid::select(int row,int col,bool extend) {
    Sheet *s=owner_->sheet(); if (!s) return;
    row=clamp(row,0,MaxRows-1); col=clamp(col,0,MaxCols-1);
    for (size_t n=0;n<s->merges.size();++n) if (s->merges[n].contains(row,col)) { row=s->merges[n].r1; col=s->merges[n].c1; break; }
    if (!extend) { anchor_row_=row; anchor_col_=col; }
    selection_={row<anchor_row_?row:anchor_row_,col<anchor_col_?col:anchor_col_,row>anchor_row_?row:anchor_row_,col>anchor_col_?col:anchor_col_};
    s->cursor_row=row; s->cursor_col=col;
    cursor_row_=row; cursor_col_=col;
    if (!extend) for (size_t n=0;n<s->merges.size();++n) if (s->merges[n].contains(row,col)) { selection_=s->merges[n]; break; }
    ensure_visible(row,col); owner_->selected(); redraw();
}
void SheetGrid::select_range(Range range) {
    if (!range.valid()) return;
    if (!finish_edit(true)) return;
    selection_=range; anchor_row_=range.r1; anchor_col_=range.c1;
    Sheet *s=owner_->sheet(); if (s) { s->cursor_row=range.r1; s->cursor_col=range.c1; }
    cursor_row_=range.r1; cursor_col_=range.c1;
    ensure_visible(range.r1,range.c1); owner_->selected(); redraw();
}
void SheetGrid::move(int dr,int dc,bool extend) {
    Sheet *s=owner_->sheet(); if (!s) return;
    int row=clamp(s->cursor_row+dr,0,MaxRows-1),col=clamp(s->cursor_col+dc,0,MaxCols-1);
    if (dr) while (row>0 && row<MaxRows-1 && !s->row_visible(row)) row+=dr>0?1:-1;
    select(row,col,extend);
}
void SheetGrid::begin_edit(const char *initial,bool replace) {
    if (owner_->is_busy()) return;
    Sheet *s=owner_->sheet(); if (!s) return;
    ensure_visible(s->cursor_row,s->cursor_col);
    Range active{s->cursor_row,s->cursor_col,s->cursor_row,s->cursor_col};
    for (size_t n=0;n<s->merges.size();++n) if (s->merges[n].contains(s->cursor_row,s->cursor_col)) { active=s->merges[n]; break; }
    int X=0,Y=0,W=0,H=0;
    for (int fr=0;fr<2;++fr) for (int fc=0;fc<2;++fc) if (rectangle(active,X,Y,W,H,fr,fc)) {
        const Cell *cell=s->get(s->cursor_row,s->cursor_col);
        input_->resize(X+1,Y+1,W>4?W-2:4,H>4?H-2:4);
        input_->textfont(FL_HELVETICA); input_->textsize(cell?cell->style.size:14);
        input_->value(initial?initial:(replace?"":(cell?cell->input.c_str():"")));
        input_->show(); input_->take_focus(); input_->position(int(strlen(input_->value()))); redraw(); return;
    }
}
bool SheetGrid::finish_edit(bool commit,int dr,int dc) {
    if (!input_->visible()) return true;
    Sheet *s=owner_->sheet(); if (!s) { input_->hide(); return true; }
    Text value; if (!value.set(input_->value())) return false;
    int row=s->cursor_row,col=s->cursor_col;
    input_->hide();
    if (commit && !owner_->commit_cell(row,col,value.c_str())) {
        input_->value(value.c_str()); input_->show(); input_->take_focus(); return false;
    }
    take_focus(); if (dr || dc) move(dr,dc); else owner_->selected(); redraw(); return true;
}
int SheetGrid::handle(int event) {
    if (event==FL_FOCUS || event==FL_UNFOCUS) { redraw(); return 1; }
    if (owner_->is_busy()) {
        switch (event) {
        case FL_PUSH: case FL_DRAG: case FL_RELEASE: case FL_KEYDOWN: case FL_KEYUP:
        case FL_SHORTCUT: case FL_MOUSEWHEEL: case FL_PASTE:
        case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: return 1;
        default: return 0;
        }
    }
    if (input_->visible() && (event==FL_KEYDOWN || event==FL_PASTE)) return input_->handle(event);
    if (event==FL_PASTE) { owner_->paste_text(Fl::event_text(),Fl::event_length()); return 1; }
    if (event==FL_MOUSEWHEEL) {
        if (!finish_edit(true)) return 1;
        Sheet *s=owner_->sheet(); if (!s) return 1;
        if (Fl::event_state(FL_CTRL)) {
            int zoom=clamp(s->zoom-Fl::event_dy()*10,25,400);
            owner_->set_busy(true);
            bool ok=owner_->editor.set_view(s->id,s->freeze_row,s->freeze_column,s->gridlines,zoom);
            owner_->set_busy(false); if (ok) owner_->changed();
        } else if (Fl::event_state(FL_SHIFT) || Fl::event_dx()) s->scroll_col=clamp(s->scroll_col+Fl::event_dx()+Fl::event_dy(),s->freeze_column?1:0,MaxCols-1);
        else s->scroll_row=clamp(s->scroll_row+Fl::event_dy()*3,0,visible_rows_.empty()?0:int(visible_rows_.size()-1));
        redraw(); return 1;
    }
    if (event==FL_PUSH) {
        if (Fl::event_x()>=x()+w()-Fl::scrollbar_size() || Fl::event_y()>=y()+h()-Fl::scrollbar_size()) return Fl_Group::handle(event);
        if (!finish_edit(true) || !owner_->commit_formula()) return 1;
        layout_cells(); take_focus();
        int row=-1,col=-1; hit(Fl::event_x(),Fl::event_y(),row,col);
        if (Fl::event_button()==FL_RIGHT_MOUSE) {
            if (row>=0 && col>=0 && !selection_.contains(row,col)) select(row,col);
            Fl_Menu_Item menu[7]{};
            const char *labels[]={T("Cut"),T("Copy"),T("Paste"),T("Paste values"),T("Clear contents"),T("Find")};
            for (int item=0;item<6;++item) menu[item].text=labels[item];
            const Fl_Menu_Item *chosen=menu->popup(Fl::event_x_root(),Fl::event_y_root());
            if (chosen) {
                int index=int(chosen-menu);
                if (index<4) owner_->act(static_cast<Command>(int(Command::Cut)+index));
                else if (index==4) { owner_->set_busy(true); bool ok=owner_->editor.clear_range(owner_->editor.book.active_id,selection_); owner_->set_busy(false); if (ok) owner_->changed(); else owner_->message(owner_->editor.error.c_str()); }
                else owner_->act(Command::Find);
            }
            return 1;
        }
        if (row<0 && col>=0) for (size_t i=0;i<columns_.size();++i) if (columns_[i].index==col && abs(Fl::event_x()-columns_[i].position-columns_[i].extent)<5) {
            resizing_=col; resize_rows_=false; resize_start_=Fl::event_x();
            resize_pixels_=resize_original_=column_pixels(col)*100/owner_->sheet()->zoom; return 1;
        }
        if (col<0 && row>=0) for (size_t i=0;i<rows_.size();++i) if (rows_[i].index==row && abs(Fl::event_y()-rows_[i].position-rows_[i].extent)<5) {
            resizing_=row; resize_rows_=true; resize_start_=Fl::event_y();
            resize_pixels_=resize_original_=row_pixels(row)*100/owner_->sheet()->zoom; return 1;
        }
        if (row<0 && col<0) { owner_->act(Command::SelectAll); return 1; }
        if (row<0) { select_range(Range{0,col,MaxRows-1,col}); return 1; }
        if (col<0) { select_range(Range{row,0,row,MaxCols-1}); return 1; }
        for (size_t ri=0;ri<rows_.size();++ri) for (size_t ci=0;ci<columns_.size();++ci) {
            if (rows_[ri].index!=selection_.r2 || columns_[ci].index!=selection_.c2) continue;
            if (abs(Fl::event_x()-columns_[ci].position-columns_[ci].extent)<5 && abs(Fl::event_y()-rows_[ri].position-rows_[ri].extent)<5) {
                fill_source_=fill_target_=selection_; filling_=true; return 1;
            }
        }
        select(row,col,Fl::event_state(FL_SHIFT)); dragging_=true;
        if (Fl::event_clicks()) { dragging_=false; begin_edit(); }
        return 1;
    }
    if (event==FL_DRAG) {
        if (resizing_>=0) {
            int coordinate=resize_rows_?Fl::event_y():Fl::event_x();
            resize_pixels_=clamp(resize_original_+(coordinate-resize_start_)*100/owner_->sheet()->zoom,12,600);
            redraw(); return 1;
        }
        if (dragging_ || filling_) {
            Sheet *s=owner_->sheet();
            if (Fl::event_y()<y()+header_height_) s->scroll_row=clamp(s->scroll_row-1,0,MaxRows-1);
            if (Fl::event_y()>y()+h()-Fl::scrollbar_size()) s->scroll_row=clamp(s->scroll_row+1,0,int(visible_rows_.size())-1);
            layout_cells(); int row=-1,col=-1;
            hit(clamp(Fl::event_x(),x()+header_width_+1,x()+w()-Fl::scrollbar_size()-1),clamp(Fl::event_y(),y()+header_height_+1,y()+h()-Fl::scrollbar_size()-1),row,col);
            if (row>=0 && col>=0) {
                if (filling_) fill_target_={row<fill_source_.r1?row:fill_source_.r1,col<fill_source_.c1?col:fill_source_.c1,row>fill_source_.r2?row:fill_source_.r2,col>fill_source_.c2?col:fill_source_.c2};
                else select(row,col,true);
                redraw();
            }
            return 1;
        }
    }
    if (event==FL_RELEASE) {
        if (resizing_>=0) {
            int index=resizing_,pixels=resize_pixels_; bool rows=resize_rows_; resizing_=-1;
            owner_->set_busy(true); bool ok=owner_->editor.resize_axis(owner_->editor.book.active_id,rows,index,1,pixels); owner_->set_busy(false);
            if (ok) owner_->changed(); else owner_->message(owner_->editor.error.c_str());
            redraw(); return 1;
        }
        if (filling_) {
            filling_=false;
            owner_->set_busy(true); bool ok=owner_->editor.fill(owner_->editor.book.active_id,fill_source_,fill_target_); owner_->set_busy(false);
            if (ok) { selection_=fill_target_; owner_->changed(); }
            else owner_->message(owner_->editor.error.c_str());
            redraw(); return 1;
        }
        if (dragging_) { dragging_=false; return 1; }
    }
    if (event==FL_KEYDOWN || event==FL_SHORTCUT) {
        int key=Fl::event_key(); bool shift=Fl::event_state(FL_SHIFT),ctrl=Fl::event_state(FL_CTRL);
        if (ctrl) {
            switch (key) {
            case 'c': owner_->act(Command::Copy); return 1;
            case 'x': owner_->act(Command::Cut); return 1;
            case 'v': owner_->act(shift?Command::PasteValues:Command::Paste); return 1;
            case 'a': owner_->act(Command::SelectAll); return 1;
            case 'd': owner_->act(Command::FillDown); return 1;
            case 'r': owner_->act(Command::FillRight); return 1;
            default: break;
            }
        }
        if (key==FL_Escape) { dragging_=filling_=false; owner_->clipboard.cut=false; redraw(); return 1; }
        if (key==FL_F+2 && !Fl::event_state(FL_CTRL|FL_ALT|FL_META)) { begin_edit(); return 1; }
        if (key==FL_Delete || key==FL_BackSpace) {
            owner_->set_busy(true); bool ok=owner_->editor.clear_range(owner_->editor.book.active_id,selection_); owner_->set_busy(false);
            if (ok) owner_->changed(); else owner_->message(owner_->editor.error.c_str()); return 1;
        }
        if (key==FL_Left) { if (ctrl) select(owner_->sheet()->cursor_row,0,shift); else move(0,-1,shift); return 1; }
        if (key==FL_Right) { if (ctrl) select(owner_->sheet()->cursor_row,owner_->sheet()->used_columns()?owner_->sheet()->used_columns()-1:0,shift); else move(0,1,shift); return 1; }
        if (key==FL_Up) { if (ctrl) select(0,owner_->sheet()->cursor_col,shift); else move(-1,0,shift); return 1; }
        if (key==FL_Down) { if (ctrl) select(owner_->sheet()->used_rows()?owner_->sheet()->used_rows()-1:0,owner_->sheet()->cursor_col,shift); else move(1,0,shift); return 1; }
        if (key==FL_Home) { select(ctrl?0:owner_->sheet()->cursor_row,0,shift); return 1; }
        if (key==FL_End) { select(ctrl?owner_->sheet()->used_rows()-1:owner_->sheet()->cursor_row,owner_->sheet()->used_columns()-1,shift); return 1; }
        if (key==FL_Page_Up || key==FL_Page_Down) { move((key==FL_Page_Up?-1:1)*(int(rows_.size())>2?int(rows_.size())-2:1),0,shift); return 1; }
        if (key==FL_Tab) { move(0,shift?-1:1); return 1; }
        if (key==FL_Enter || key==FL_KP_Enter) { move(shift?-1:1,0); return 1; }
        if (!ctrl && !Fl::event_state(FL_ALT) && Fl::event_length()>0 && static_cast<unsigned char>(Fl::event_text()[0])>=32) {
            begin_edit(nullptr,true); return input_->handle(event);
        }
    }
    return Fl_Group::handle(event);
}
}
