#include "window.h"
#include "grid.h"
#include "formula.h"
#include "../fltk_support.h"
#include <FL/Fl.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Check_Button.H>
#include <FL/fl_draw.H>
#include <FL/fl_ask.H>
#include <FL/fl_show_colormap.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/x.H>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
namespace rx {
namespace {
int clamp(int value,int low,int high) { return value<low?low:value>high?high:value; }
uint64_t monotonic_ms() {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC,&time)) return 0;
    return uint64_t(time.tv_sec)*1000+uint64_t(time.tv_nsec)/1000000;
}
unsigned long selection_owner() {
    using AtomFn=Atom(*)(Display *,const char *,Bool);
    using OwnerFn=Window(*)(Display *,Atom);
    static AtomFn atom=reinterpret_cast<AtomFn>(dlsym(RTLD_DEFAULT,"XInternAtom"));
    static OwnerFn owner=reinterpret_cast<OwnerFn>(dlsym(RTLD_DEFAULT,"XGetSelectionOwner"));
    return atom && owner && fl_display?owner(fl_display,atom(fl_display,"CLIPBOARD",False)):0;
}
bool has_suffix(const char *path,const char *suffix) {
    size_t n=strlen(path),m=strlen(suffix); return n>=m && !strcasecmp(path+n-m,suffix);
}
void remove_recovery_copy(Text &path,int &descriptor) {
    if (!path.empty()) {
        if (!unlink(path.c_str()) || errno==ENOENT) {
            char lock[4096]; snprintf(lock,sizeof(lock),"%s.lock",path.c_str()); unlink(lock);
        }
        path.clear();
    }
    unlock_recovery_path(descriptor);
}
bool text_contains(const char *text,const char *query,bool match_case) {
    if (match_case) return strstr(text,query)!=nullptr;
    size_t n=strlen(query);
    for (const char *at=text;*at;++at) if (!strncasecmp(at,query,n)) return true;
    return !n;
}
Range data_range(Sheet *sheet,Range selected) {
    if (selected.r1!=selected.r2 || selected.c1!=selected.c2) return selected;
    return Range{0,0,sheet->used_rows()>0?sheet->used_rows()-1:0,sheet->used_columns()>0?sheet->used_columns()-1:0};
}
struct ModalResult { Fl_Window *window; int result=0; };
struct ModalAction { ModalResult *result; int choice; };
void modal_button(Fl_Widget *widget,void *context) {
    (void)widget;
    ModalAction *action=static_cast<ModalAction *>(context);
    action->result->result=action->choice; action->result->window->hide();
}
void modal_cancel(Fl_Widget *widget,void *context) {
    ModalResult *result=static_cast<ModalResult *>(context); result->result=0; widget->hide();
}
void wait_modal(Fl_Window &window) { reliefos_fltk::style(&window); window.set_modal(); window.show(); while (window.shown()) Fl::wait(); }
}
ExcelWindow::ExcelWindow() : Fl_Double_Window(1120,720,"ReliefExcel") {
    size_range(840,540); callback(close_cb,this);
    if (!editor.book.sheets.size()) editor.book.init();
    if (sheet() && sheet()->name=="Sheet1") sheet()->name.set(T("Sheet1"));
    saved_revision_=editor.book.revision;
    editor.progress=progress; editor.progress_context=this;
    begin();
    ribbon_tabs_=new Fl_Group(0,0,w(),34);
    file_=new Fl_Menu_Button(7,3,64,28,T("&File")); file_->box(FL_THIN_UP_BOX);
    auto addfile=[this](const char *label,Command command,int shortcut=0) { file_->add(label,shortcut,dispatch,reinterpret_cast<void *>(intptr_t(command)+1)); };
    addfile(T("New"),Command::New,FL_CTRL+'n'); addfile(T("Open..."),Command::Open,FL_CTRL+'o');
    addfile(T("Save"),Command::Save,FL_CTRL+'s'); addfile(T("Save as..."),Command::SaveAs,FL_CTRL+FL_SHIFT+'s');
    addfile(T("Import CSV..."),Command::Import); addfile(T("Export CSV..."),Command::Export);
    addfile(T("Budget template"),Command::Template); addfile(T("Exit"),Command::Quit);
    const char *labels[]={T("Home tab"),T("Insert"),T("Formulas"),T("Data"),T("View")};
    for (int i=0;i<5;++i) {
        tabs_[i]=new Fl_Button(80+i*76,3,72,28,labels[i]); tabs_[i]->callback(ribbon_tab_cb,reinterpret_cast<void *>(intptr_t(i)));
        tabs_[i]->clear_visible_focus();
    }
    Fl_Box *spacer=new Fl_Box(468,0,w()-660,34);
    const char *quick[]={T("Save"),T("Undo"),T("Redo")};
    Command actions[]={Command::Save,Command::Undo,Command::Redo};
    for (int i=0;i<3;++i) {
        Fl_Button *button=new Fl_Button(w()-190+i*62,4,58,26,quick[i]);
        button->callback(dispatch,reinterpret_cast<void *>(intptr_t(actions[i])+1)); button->clear_visible_focus();
    }
    ribbon_tabs_->resizable(spacer); ribbon_tabs_->end();
    ribbon_=new Fl_Group(0,34,w(),100);
    ribbon_scroll_=new Fl_Scroll(0,34,w(),100); ribbon_scroll_->type(Fl_Scroll::HORIZONTAL); ribbon_scroll_->scrollbar_size(12);
    build_ribbon(); ribbon_scroll_->end(); ribbon_->resizable(ribbon_scroll_); ribbon_->end();
    formula_group_=new Fl_Group(0,136,w(),34);
    name_=new Fl_Input(8,139,106,28); name_->tooltip(T("Cell address, for example A1 or C25")); name_->when(FL_WHEN_ENTER_KEY_ALWAYS); name_->callback(name_cb,this);
    Fl_Button *fx=new Fl_Button(120,139,38,28,"fx"); fx->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Function)+1)); fx->tooltip(T("Insert function"));
    formula_=new Fl_Input(164,139,w()-172,28); formula_->maximum_size(MaxTextBytes); formula_->when(FL_WHEN_ENTER_KEY_ALWAYS); formula_->callback(formula_cb,this); formula_->tooltip(T("Edit cell content or formula; Enter applies the change"));
    formula_group_->resizable(formula_); formula_group_->end();
    grid=new SheetGrid(0,172,w(),h()-232,this);
    bottom_=new Fl_Group(0,h()-60,w(),60);
    sheet_scroll_=new Fl_Scroll(0,h()-60,w(),34); sheet_scroll_->type(Fl_Scroll::HORIZONTAL); sheet_scroll_->scrollbar_size(10); sheet_scroll_->end();
    status_=new Fl_Box(8,h()-24,w()/2-12,22,T("Ready")); status_->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); status_->labelsize(13);
    statistics_=new Fl_Box(w()/2,h()-24,w()/2-8,22); statistics_->align(FL_ALIGN_RIGHT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); statistics_->labelsize(13);
    cancel_=new Fl_Button(w()-88,h()-24,80,22,T("Cancel")); cancel_->callback([](Fl_Widget *,void *context){ static_cast<ExcelWindow *>(context)->canceled_=true; },this); cancel_->hide();
    bottom_->end(); end(); resizable(grid);
    build_sheet_tabs(); select_ribbon(0); selected();
    int X,Y,W,H; Fl::screen_work_area(X,Y,W,H);
    int width=w()<W-24?w():W-24,height=h()<H-48?h():H-48;
    if (width>=840 && height>=540) resize(X+(W-width)/2,Y+(H-height)/2,width,height);
    Fl::add_timeout(60,recovery_tick,this);
}
ExcelWindow::~ExcelWindow() {
    Fl::remove_timeout(recovery_tick,this);
    discard_recovery();
}
void ExcelWindow::build_ribbon() {
    const int Y=34;
    auto pane=[&](int index) { panes_[index]=new Fl_Group(0,Y,1100,84); };
    auto button=[&](int X,int row,int W,const char *label,Command command,const char *tip=nullptr) {
        Fl_Button *b=new Fl_Button(X,Y+8+row*29,W,26,label);
        b->callback(dispatch,reinterpret_cast<void *>(intptr_t(command)+1)); b->clear_visible_focus();
        if (tip) b->tooltip(tip);
        if (command==Command::Bold || command==Command::Italic || command==Command::Wrap || command==Command::Formulas || command==Command::Gridlines) b->type(FL_TOGGLE_BUTTON);
        return b;
    };
    auto group=[&](int X,int W,const char *label) {
        Fl_Box *caption=new Fl_Box(X,Y+68,W,16,label); caption->labelsize(11); caption->labelcolor(fl_rgb_color(85,94,88));
        Fl_Box *line=new Fl_Box(X+W-3,Y+6,1,57); line->box(FL_FLAT_BOX); line->color(fl_rgb_color(210,215,211));
    };
    pane(0);
    button(8,0,64,T("Paste"),Command::Paste,T("Ctrl+V")); button(78,0,58,T("Copy"),Command::Copy,T("Ctrl+C")); button(142,0,58,T("Cut"),Command::Cut,T("Ctrl+X"));
    button(8,1,92,T("Paste values"),Command::PasteValues); button(106,1,94,T("Fill down"),Command::FillDown,T("Ctrl+D")); group(4,205,T("Clipboard"));
    size_=new Fl_Choice(214,Y+8,66,26); size_->add("10|12|14|16|18|20|24|28|32|36"); size_->value(2); size_->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Size)+1)); size_->tooltip(T("Font size"));
    button(286,0,46,T("Bold"),Command::Bold); button(338,0,46,T("Italic"),Command::Italic); button(390,0,72,T("Borders"),Command::Borders);
    button(214,1,80,T("Text color"),Command::TextColor); button(300,1,80,T("Fill color"),Command::FillColor); button(386,1,76,T("No borders"),Command::NoBorders); group(210,260,T("Font"));
    button(476,0,58,T("Left"),Command::AlignLeft); button(540,0,58,T("Center"),Command::AlignCenter); button(604,0,58,T("Right"),Command::AlignRight);
    button(476,1,90,T("Wrap text"),Command::Wrap); button(572,1,90,T("Merge"),Command::Merge); group(472,198,T("Alignment"));
    button(676,0,82,T("General"),Command::General); button(764,0,82,T("Number"),Command::Number);
    button(676,1,82,T("Percent"),Command::Percent); button(764,1,82,T("Currency"),Command::Currency); group(672,182,T("Number format"));
    button(860,0,100,T("Unmerge"),Command::Unmerge); button(860,1,100,T("Fill right"),Command::FillRight,T("Ctrl+R")); group(856,112,T("Cells"));
    panes_[0]->end();
    pane(1);
    button(12,0,120,T("Insert row"),Command::InsertRow); button(12,1,120,T("Delete row"),Command::DeleteRow); group(6,136,T("Rows"));
    button(150,0,132,T("Insert column"),Command::InsertColumn); button(150,1,132,T("Delete column"),Command::DeleteColumn); group(144,146,T("Columns"));
    button(298,0,124,T("New sheet"),Command::AddSheet); button(298,1,124,T("Delete sheet"),Command::DeleteSheet);
    button(430,0,124,T("Rename sheet"),Command::RenameSheet); button(430,1,124,T("Move sheet left"),Command::MoveLeft); button(562,1,124,T("Move sheet right"),Command::MoveRight); group(292,402,T("Worksheets"));
    panes_[1]->end();
    pane(2);
    button(12,0,132,T("AutoSum"),Command::AutoSum); button(12,1,132,T("Insert function"),Command::Function); group(6,146,T("Functions"));
    button(160,0,156,T("Recalculate"),Command::Recalculate); button(160,1,156,T("Show formulas"),Command::Formulas); group(154,170,T("Calculation"));
    Fl_Box *examples=new Fl_Box(340,Y+10,660,48,T("Examples: =SUM(B2:B20)    =IF(C2>0,C2,0)")); examples->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE); examples->labelsize(14); panes_[2]->end();
    pane(3);
    button(12,0,126,T("Sort ascending"),Command::SortAscending); button(12,1,126,T("Sort descending"),Command::SortDescending); group(6,140,T("Sort"));
    button(154,0,126,T("Filter"),Command::Filter); button(154,1,126,T("Clear filters"),Command::ClearFilter); group(148,140,T("Filter"));
    button(296,0,126,T("Find"),Command::Find,T("Ctrl+F")); button(296,1,126,T("Replace"),Command::Replace,T("Ctrl+H")); group(290,140,T("Find and replace"));
    button(438,0,132,T("Import CSV"),Command::Import); button(438,1,132,T("Export CSV"),Command::Export); group(432,146,T("Data exchange")); panes_[3]->end();
    pane(4);
    button(12,0,132,T("Freeze first row"),Command::FreezeRow); button(150,0,140,T("Freeze first column"),Command::FreezeColumn);
    button(12,1,132,T("Freeze both"),Command::FreezeBoth); button(150,1,140,T("Unfreeze"),Command::Unfreeze); group(6,292,T("Freeze panes"));
    button(306,0,132,T("Gridlines"),Command::Gridlines); button(306,1,132,T("Zoom..."),Command::Zoom); group(300,146,T("Display")); panes_[4]->end();
}
void ExcelWindow::dispatch(Fl_Widget *widget,void *data) { static_cast<ExcelWindow *>(widget->window())->act(static_cast<Command>(intptr_t(data)-1)); }
void ExcelWindow::ribbon_tab_cb(Fl_Widget *widget,void *data) { static_cast<ExcelWindow *>(widget->window())->select_ribbon(int(intptr_t(data))); }
void ExcelWindow::select_ribbon(int index) {
    if (busy_ || !grid || !grid->finish_edit(true) || !commit_formula()) return;
    ribbon_index_=clamp(index,0,4);
    for (int i=0;i<5;++i) { if (i==ribbon_index_) panes_[i]->show(); else panes_[i]->hide(); tabs_[i]->value(i==ribbon_index_); }
    ribbon_scroll_->scroll_to(0,0); ribbon_->redraw(); grid->take_focus();
}
void ExcelWindow::build_sheet_tabs() {
    Fl_Group *previous=Fl_Group::current();
    for (size_t i=0;i<sheet_buttons_.size();++i) { sheet_scroll_->remove(sheet_buttons_[i]); delete sheet_buttons_[i]; }
    sheet_buttons_.clear(); sheet_scroll_->begin(); int X=8;
    for (size_t i=0;i<editor.book.sheets.size();++i) {
        Sheet &s=editor.book.sheets[i]; fl_font(FL_HELVETICA,14);
        int width=clamp(int(fl_width(s.name.c_str()))+26,86,196);
        Fl_Button *b=new Fl_Button(X,sheet_scroll_->y()+2,width,22); b->copy_label(s.name.c_str()); b->tooltip(s.name.c_str());
        b->callback(sheet_cb,reinterpret_cast<void *>(uintptr_t(s.id))); b->clear_visible_focus(); b->align(FL_ALIGN_INSIDE|FL_ALIGN_CLIP); b->value(s.id==editor.book.active_id);
        if (s.id==editor.book.active_id) { b->labelcolor(fl_rgb_color(25,100,55)); b->labelfont(FL_HELVETICA_BOLD); }
        sheet_buttons_.push(b); X+=width+4;
    }
    Fl_Button *add=new Fl_Button(X,sheet_scroll_->y()+2,30,22,"+"); add->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::AddSheet)+1)); add->tooltip(T("New sheet")); sheet_buttons_.push(add);
    sheet_scroll_->end(); Fl_Group::current(previous); sheet_scroll_->redraw();
}
void ExcelWindow::sheet_cb(Fl_Widget *widget,void *data) {
    ExcelWindow *w=static_cast<ExcelWindow *>(widget->window());
    if (w->busy_ || !w->grid->finish_edit(true) || !w->commit_formula()) return;
    w->editor.book.active_id=uint32_t(uintptr_t(data)); w->build_sheet_tabs(); w->grid->refresh(true); w->selected(); w->grid->take_focus();
}
void ExcelWindow::name_cb(Fl_Widget *,void *context) {
    ExcelWindow *w=static_cast<ExcelWindow *>(context); int row=0,col=0;
    if (!w->busy_ && parse_address(w->name_->value(),row,col)) { if (w->commit_formula()) { w->grid->select(row,col); w->grid->take_focus(); } }
    else w->message(T("Enter an address between A1 and IV10000"));
}
void ExcelWindow::formula_cb(Fl_Widget *,void *context) {
    ExcelWindow *w=static_cast<ExcelWindow *>(context);
    if (w->busy_ || Fl::compose_state) return;
    if (w->commit_formula()) { w->grid->take_focus(); w->grid->move(1,0); }
}
void ExcelWindow::close_cb(Fl_Widget *,void *context) {
    ExcelWindow *w=static_cast<ExcelWindow *>(context);
    if (w->busy_) { w->canceled_=true; return; }
    if (w->grid->finish_edit(true) && w->commit_formula() && w->confirm_discard()) w->hide();
}
void ExcelWindow::message(const char *text) { status_->copy_label(T(text?text:"")); status_->redraw(); }
void ExcelWindow::set_busy(bool busy) {
    if (busy) { if (busy_depth_++) return; canceled_=false; progress_percent_=-1; progress_tick_=monotonic_ms(); }
    else { if (!busy_depth_ || --busy_depth_) return; }
    busy_=busy;
    if (busy) { ribbon_->deactivate(); ribbon_tabs_->deactivate(); formula_group_->deactivate(); sheet_scroll_->deactivate(); statistics_->hide(); cancel_->show(); }
    else { ribbon_->activate(); ribbon_tabs_->activate(); formula_group_->activate(); sheet_scroll_->activate(); statistics_->show(); cancel_->hide(); }
    redraw();
}
bool ExcelWindow::progress(void *context,unsigned done,unsigned total) {
    ExcelWindow *w=static_cast<ExcelWindow *>(context);
    unsigned percent=total?unsigned(uint64_t(done)*100/total):0;
    uint64_t now=monotonic_ms();
    // Short edits finish without pumping another input event inside a commit.
    // Long operations still repaint and accept cancellation at bounded intervals.
    if (now && w->progress_tick_ && now-w->progress_tick_<80) return !w->canceled_;
    if (!now && w->progress_percent_==int(percent)) return !w->canceled_;
    w->progress_tick_=now;
    w->progress_percent_=int(percent);
    char label[96]; snprintf(label,sizeof(label),"%s %u%%",T("Working..."),percent);
    w->status_->copy_label(label); Fl::check(); return !w->canceled_;
}
bool ExcelWindow::commit_cell(int row,int col,const char *text,bool literal) {
    if (busy_) return false;
    const Cell *cell=editor.book.get(editor.book.active_id,row,col);
    if ((!cell && !*text) || (cell && cell->input==text)) return true;
    set_busy(true); bool ok=editor.set_cell(editor.book.active_id,row,col,text,literal); set_busy(false);
    if (ok) changed(); else { message(editor.error.c_str()); fl_alert("%s",T(editor.error.c_str())); }
    return ok;
}
bool ExcelWindow::commit_formula() {
    if (busy_) return false;
    if (updating_ || !formula_ || !formula_->changed()) return true;
    Sheet *s=sheet(); if (!s) return false;
    Text value; if (!value.set(formula_->value())) return false;
    formula_->clear_changed();
    bool ok=commit_cell(s->cursor_row,s->cursor_col,value.c_str());
    if (!ok) formula_->set_changed();
    return ok;
}
void ExcelWindow::selected() {
    Sheet *s=sheet(); if (!s || !grid || !formula_ || busy_) return;
    updating_=true;
    const Cell *cell=s->get(s->cursor_row,s->cursor_col);
    char column[12],address[32]; column_label(s->cursor_col,column,sizeof(column)); snprintf(address,sizeof(address),"%s%d",column,s->cursor_row+1);
    name_->value(address); name_->clear_changed();
    formula_->value(cell?cell->input.c_str():""); formula_->clear_changed();
    Range range=grid->selection(); double sum=0; unsigned count=0,numeric=0;
    for (size_t i=0;i<s->cells.size();++i) {
        Cell &c=s->cells[i]; if (!range.contains(c.row,c.col) || c.input.empty()) continue;
        ++count; Value value=evaluate(editor.book,s->id,c.row,c.col);
        if (value.kind==ValueKind::Number) { sum+=value.number; ++numeric; }
    }
    char summary[192];
    if (numeric) snprintf(summary,sizeof(summary),"%s %u     %s %.10g     %s %.10g     %d%%",T("Count:"),count,T("Sum:"),sum,T("Average:"),sum/numeric,s->zoom);
    else snprintf(summary,sizeof(summary),"%s %u     %d%%",T("Count:"),count,s->zoom);
    statistics_->copy_label(summary);
    const int sizes[]={10,12,14,16,18,20,24,28,32,36};
    if (size_) for (int i=0;i<10;++i) if (sizes[i]==(cell?cell->style.size:14)) size_->value(i);
    auto sync_buttons=[&](auto &&self,Fl_Group *group)->void {
        for (int i=0;i<group->children();++i) {
            Fl_Widget *child=group->child(i);
            if (child->as_group()) self(self,child->as_group());
            if (child->callback()!=dispatch) continue;
            Command command=static_cast<Command>(intptr_t(child->user_data())-1);
            if (command==Command::Bold || command==Command::Italic || command==Command::Wrap || command==Command::Formulas || command==Command::Gridlines) {
                bool enabled=command==Command::Bold?(cell&&cell->style.bold):command==Command::Italic?(cell&&cell->style.italic):command==Command::Wrap?(cell&&cell->style.wrap):command==Command::Formulas?show_formulas:s->gridlines;
                static_cast<Fl_Button *>(child)->value(enabled);
            }
            if (command==Command::Undo) { if (editor.can_undo()) child->activate(); else child->deactivate(); }
            if (command==Command::Redo) { if (editor.can_redo()) child->activate(); else child->deactivate(); }
        }
    };
    sync_buttons(sync_buttons,ribbon_); sync_buttons(sync_buttons,ribbon_tabs_);
    updating_=false;
}
void ExcelWindow::changed(bool sheet_list) {
    set_busy(true); bool complete=recalculate(editor.book,progress,this); set_busy(false);
    if (sheet_list) build_sheet_tabs();
    grid->refresh(sheet_list); selected();
    const char *base=path_.empty()?T("Untitled"):strrchr(path_.c_str(),'/');
    if (!path_.empty()) base=base?base+1:path_.c_str();
    char title[1024]; snprintf(title,sizeof(title),"%s%s — ReliefExcel",editor.book.revision!=saved_revision_?"* ":"",base); copy_label(title);
    message(complete?T("Ready"):T("Calculation canceled; results will update when accessed"));
}
bool ExcelWindow::confirm_discard() {
    if (editor.book.revision==saved_revision_) return true;
    int choice=fl_choice("%s",T("Cancel"),T("Save"),T("Discard changes"),T("Save changes to this workbook?"));
    if (choice==1) return save(false);
    return choice==2;
}
void ExcelWindow::discard_recovery() {
    remove_recovery_copy(recovery_path_,recovery_descriptor_);
    remove_recovery_copy(recovered_from_,recovered_descriptor_);
    recovery_revision_=0;
    clipboard.cut=false; clipboard_owner=0;
}
bool ExcelWindow::choose_path(Text &output,bool saving,bool csv) {
    Fl_File_Chooser chooser(path_.empty()?".":path_.c_str(),csv?"*.{csv,CSV}":(saving?"*.{rexl,REXL}":"*.{rexl,csv,REXL,CSV}"),saving?Fl_File_Chooser::CREATE:Fl_File_Chooser::SINGLE,saving?T("Save workbook"):T("Open workbook"));
    chooser.preview(0); chooser.show(); while (chooser.shown()) Fl::wait();
    const char *chosen=chooser.value(); if (!chosen || !*chosen) return false;
    if (!output.set(chosen)) return false;
    if (saving && !has_suffix(chosen,csv?".csv":".rexl")) {
        size_t n=strlen(chosen); char *bytes=static_cast<char *>(malloc(n+6)); if (!bytes) return false;
        snprintf(bytes,n+6,"%s%s",chosen,csv?".csv":".rexl"); bool ok=output.set(bytes); free(bytes); return ok;
    }
    return true;
}
bool ExcelWindow::save(bool as_new) {
    Text target=path_,error; FileStamp expected;
    if (as_new || target.empty()) {
        if (!choose_path(target,true)) return false;
        if (!file_stamp(target.c_str(),expected,error)) { fl_alert("%s",T(error.c_str())); return false; }
        if (expected.exists && fl_choice("%s",T("Cancel"),T("Replace"),nullptr,T("This file already exists. Replace it?"))!=1) return false;
    } else expected=stamp_;
    set_busy(true); bool ok=save_workbook(target.c_str(),editor.book,error,&expected,progress,this); set_busy(false);
    if (!ok) {
        FileStamp current; Text stamp_error;
        if (file_stamp(target.c_str(),current,stamp_error) && !same_stamp(current,expected)) {
            int choice=fl_choice("%s",T("Cancel"),T("Save as..."),T("Replace"),T("The file changed outside ReliefExcel. Choose how to save."));
            if (choice==1) return save(true);
            if (choice!=2) { message(T("Save canceled; original file preserved")); return false; }
            set_busy(true); ok=save_workbook(target.c_str(),editor.book,error,&current,progress,this); set_busy(false);
        }
    }
    if (!ok) { fl_alert("%s",T(error.c_str())); return false; }
    path_=target; saved_revision_=editor.book.revision; file_stamp(path_.c_str(),stamp_,error);
    remove_recovery_copy(recovered_from_,recovered_descriptor_);
    remove_recovery_copy(recovery_path_,recovery_descriptor_);
    recovery_revision_=0;
    changed(); message(T("Workbook saved")); return true;
}
void ExcelWindow::open(const char *filename,bool csv) {
    if (!confirm_discard()) return;
    Workbook replacement; Text error; bool ok=false; char delimiter=',';
    for (;;) {
        set_busy(true);
        ok=csv?import_csv(filename,replacement,error,delimiter,progress,this):load_workbook(filename,replacement,error,progress,this);
        set_busy(false);
        if (!ok) { fl_alert("%s",T(error.c_str())); return; }
        if (!csv) break;
        Fl_Double_Window dialog(720,420,T("Import CSV")); ModalResult result{&dialog};
        dialog.callback(modal_cancel,&result);
        Fl_Choice separators(120,16,190,28,T("Separator:")); separators.add(T("Comma|Semicolon|Tab")); separators.value(delimiter==','?0:delimiter==';'?1:2);
        char detail[160]; Sheet *s=replacement.sheet(replacement.active_id);
        snprintf(detail,sizeof(detail),"%s %d    %s %d",T("Rows:"),s?s->used_rows():0,T("Columns:"),s?s->used_columns():0);
        Fl_Box dimensions(330,16,374,28,detail); dimensions.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
        Fl_Multiline_Output preview(16,60,688,282); preview.textfont(FL_COURIER); preview.textsize(14);
        char text[8192]; size_t used=0; text[0]=0;
        if (s) for (int row=0;row<s->used_rows() && row<9;++row) {
            for (int col=0;col<s->used_columns() && col<6;++col) {
                const Cell *cell=s->get(row,col); const char *value=cell?cell->input.c_str():"";
                size_t length=strlen(value); if (length>120) { length=120; while (length && (static_cast<unsigned char>(value[length])&0xc0)==0x80) --length; }
                if (used+length+8<sizeof(text)) { memcpy(text+used,value,length); used+=length; text[used++]='\t'; }
            }
            if (used+2<sizeof(text)) text[used++]='\n';
        }
        text[used]=0; preview.value(text);
        Fl_Box note(16,346,688,22,T("Preview of the first rows. Text and leading zeros are preserved; formulas are not executed.")); note.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); note.labelsize(12);
        Fl_Button cancel(400,378,92,28,T("Cancel")),refresh(500,378,92,28,T("Preview")),accept(600,378,104,28,T("Import"));
        ModalAction actions[]={{&result,0},{&result,2},{&result,1}};
        cancel.callback(modal_button,&actions[0]); refresh.callback(modal_button,&actions[1]); accept.callback(modal_button,&actions[2]);
        dialog.end(); wait_modal(dialog);
        if (!result.result) return;
        char next=separators.value()==0?',':separators.value()==1?';':'\t';
        if (result.result==1 && delimiter==next) break;
        delimiter=next;
    }
    if (!editor.replace_book(static_cast<Workbook &&>(replacement))) { fl_alert("%s",T(editor.error.c_str())); return; }
    discard_recovery();
    if (csv) { path_.clear(); stamp_=FileStamp{}; saved_revision_=0; }
    else { path_.set(filename); saved_revision_=editor.book.revision; file_stamp(filename,stamp_,error); }
    changed(true); grid->take_focus(); message(csv?T("CSV imported; save as a native workbook to keep formulas and formatting"):T("Workbook opened"));
}
void ExcelWindow::load_initial(const char *filename) { open(filename,has_suffix(filename,".csv")); }
bool ExcelWindow::own_clipboard() const { return clipboard_owner && selection_owner()==clipboard_owner; }
void ExcelWindow::paste_text(const char *bytes,size_t length) {
    if (busy_ || !sheet() || !bytes || !length) return;
    bool ok=false; set_busy(true);
    if (own_clipboard() && clipboard_text.size()==length && !memcmp(clipboard_text.c_str(),bytes,length)) {
        ok=editor.paste(editor.book.active_id,sheet()->cursor_row,sheet()->cursor_col,clipboard,paste_values_);
    } else {
        Workbook imported; Text error;
        if (decode_delimited(bytes,length,imported,error,'\t')) {
            Sheet *s=imported.sheet(imported.active_id); Clipboard external;
            if (s) {
                external.range={0,0,s->used_rows()?s->used_rows()-1:0,s->used_columns()?s->used_columns()-1:0};
                if (external.cells.clone_from(s->cells)) ok=editor.paste(editor.book.active_id,sheet()->cursor_row,sheet()->cursor_col,external,true);
                else editor.error.set("Not enough memory");
            }
        } else editor.error=error;
    }
    set_busy(false); paste_values_=false;
    if (ok) changed(); else { message(editor.error.c_str()); fl_alert("%s",T(editor.error.c_str())); }
}
void ExcelWindow::function_dialog() {
    static const char *functions[]={"SUM","AVERAGE","MIN","MAX","COUNT","COUNTA","IF","ROUND","ABS","COUNTIF","SUMIF"};
    const char *help[]={T("Adds numeric cells in a range."),T("Returns the mean of numeric cells."),T("Returns the smallest number."),T("Returns the largest number."),T("Counts numeric cells."),T("Counts nonempty cells."),T("IF(condition, value_if_true, value_if_false)"),T("ROUND(number, decimal_places)"),T("ABS(number) returns its absolute value."),T("COUNTIF(range, criteria)"),T("SUMIF(range, criteria, sum_range)")};
    Fl_Double_Window dialog(620,210,T("Insert function")); ModalResult result{&dialog}; dialog.callback(modal_cancel,&result);
    Fl_Choice choice(124,20,460,30,T("Function:")); for (const char *name:functions) choice.add(name); choice.value(0);
    Fl_Box description(24,66,560,60,help[0]); description.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_WRAP);
    struct Help { const char **texts; Fl_Box *box; } state{help,&description};
    choice.callback([](Fl_Widget *widget,void *context){ Help *h=static_cast<Help *>(context); h->box->copy_label(h->texts[static_cast<Fl_Choice *>(widget)->value()]); },&state);
    Fl_Button cancel(380,162,98,30,T("Cancel")),insert(486,162,98,30,T("Insert")); ModalAction actions[]={{&result,0},{&result,1}};
    cancel.callback(modal_button,&actions[0]); insert.callback(modal_button,&actions[1]); dialog.end(); wait_modal(dialog);
    if (result.result) {
        char input[160]; const char *name=functions[choice.value()];
        if (!strcmp(name,"IF")) snprintf(input,sizeof(input),"=IF(A1>0,A1,0)");
        else if (!strcmp(name,"ROUND")) snprintf(input,sizeof(input),"=ROUND(A1,2)");
        else if (!strcmp(name,"ABS")) snprintf(input,sizeof(input),"=ABS(A1)");
        else if (!strcmp(name,"COUNTIF")) snprintf(input,sizeof(input),"=COUNTIF(A1:A10,\">0\")");
        else if (!strcmp(name,"SUMIF")) snprintf(input,sizeof(input),"=SUMIF(A1:A10,\">0\",B1:B10)");
        else snprintf(input,sizeof(input),"=%s(A1:A10)",name);
        grid->begin_edit(input);
    }
}
void ExcelWindow::data_dialog(bool filter,bool descending) {
    Sheet *s=sheet(); if (!s) return;
    Range range=data_range(s,grid->selection());
    Fl_Double_Window dialog(580,filter?260:200,filter?T("Filter"):T("Sort")); ModalResult result{&dialog}; dialog.callback(modal_cancel,&result);
    Fl_Choice column(140,20,414,30,T("Column:"));
    for (int col=range.c1;col<=range.c2;++col) {
        char label[256],letter[16]; column_label(col,letter,sizeof(letter)); const Cell *header=s->get(range.r1,col);
        snprintf(label,sizeof(label),"%s  %s",letter,header?header->input.c_str():"");
        // FLTK menu metacharacters in cell text must never create submenus.
        for (char *p=label;*p;++p) if (*p=='/' || *p=='|' || *p=='&' || *p=='@') *p=' ';
        column.add(label);
    }
    column.value(0); Fl_Check_Button header(140,60,414,26,T("First row is a header")); header.value(1);
    Fl_Choice condition(140,96,414,28,T("Condition:")); condition.add(T("Contains|Equals|Greater than|Less than|Not empty")); condition.value(0);
    Fl_Input query(140,134,414,28,T("Value:")); query.maximum_size(1024);
    if (!filter) { condition.hide(); query.hide(); }
    Fl_Box notice(20,filter?176:94,534,24,filter?T("Filtering hides rows and preserves their contents."):T("Rows and their formatting move together within the selected range.")); notice.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); notice.labelsize(12);
    int Y=dialog.h()-48; Fl_Button cancel(346,Y,98,30,T("Cancel")),apply(456,Y,98,30,T("Apply")); ModalAction actions[]={{&result,0},{&result,1}};
    cancel.callback(modal_button,&actions[0]); apply.callback(modal_button,&actions[1]); dialog.end(); wait_modal(dialog);
    if (!result.result) return;
    set_busy(true);
    bool ok=filter?editor.set_filter(s->id,range,range.c1+column.value(),query.value(),condition.value(),header.value()):editor.sort(s->id,range,range.c1+column.value(),descending,header.value());
    set_busy(false); if (ok) changed(); else fl_alert("%s",T(editor.error.c_str()));
}
bool ExcelWindow::find_next(const char *find,bool match_case) {
    Sheet *s=sheet(); if (!s || !find || !*find) return false;
    size_t start=0;
    while (start<s->cells.size() && (s->cells[start].row<s->cursor_row || (s->cells[start].row==s->cursor_row && s->cells[start].col<=s->cursor_col))) ++start;
    for (size_t step=0;step<s->cells.size();++step) {
        Cell &cell=s->cells[(start+step)%s->cells.size()];
        if (text_contains(cell.input.c_str(),find,match_case)) { grid->select(cell.row,cell.col); return true; }
    }
    return false;
}
void ExcelWindow::search_dialog(bool replacing) {
    Fl_Double_Window dialog(620,replacing?226:186,replacing?T("Find and replace"):T("Find")); ModalResult result{&dialog}; dialog.callback(modal_cancel,&result);
    Fl_Input find(126,20,470,30,T("Find:")); find.value(search_.c_str()); find.maximum_size(1024);
    Fl_Input replace(126,62,470,30,T("Replace with:")); replace.maximum_size(1024); if (!replacing) replace.hide();
    Fl_Check_Button match(126,replacing?104:64,470,26,T("Match case")); match.value(search_case_);
    Fl_Button cancel(20,dialog.h()-48,94,30,T("Close")),next(126,dialog.h()-48,142,30,T("Find next")),all(280,dialog.h()-48,146,30,T("Replace all")),one(438,dialog.h()-48,158,30,T("Replace current"));
    ModalAction actions[]={{&result,0},{&result,1},{&result,2},{&result,3}};
    cancel.callback(modal_button,&actions[0]); next.callback(modal_button,&actions[1]); all.callback(modal_button,&actions[2]); one.callback(modal_button,&actions[3]);
    if (!replacing) { all.hide(); one.hide(); }
    dialog.end();
    for (;;) {
        result.result=0; wait_modal(dialog); if (!result.result) break;
        if (!*find.value()) { fl_alert("%s",T("Enter text to find")); continue; }
        search_.set(find.value()); search_case_=match.value();
        if (result.result==1) { if (!find_next(search_.c_str(),search_case_)) fl_alert("%s",T("No matching cell found")); }
        else if (result.result==2) {
            unsigned count=0; set_busy(true); bool ok=editor.replace_all(editor.book.active_id,find.value(),replace.value(),match.value(),&count); set_busy(false);
            if (ok) { changed(); char note[128]; snprintf(note,sizeof(note),"%s %u",T("Replacements:"),count); message(note); } else fl_alert("%s",T(editor.error.c_str()));
        } else {
            Sheet *s=sheet(); const Cell *cell=s->get(s->cursor_row,s->cursor_col);
            if (!cell || !text_contains(cell->input.c_str(),find.value(),match.value())) { if (!find_next(find.value(),match.value())) fl_alert("%s",T("No matching cell found")); continue; }
            // Use the same replacement command semantics on a temporary single-cell book.
            Editor single; single.set_cell(single.book.active_id,0,0,cell->input.c_str(),cell->literal);
            unsigned count=0;
            if (single.replace_all(single.book.active_id,find.value(),replace.value(),match.value(),&count)) {
                const Cell *updated=single.book.get(single.book.active_id,0,0);
                if (commit_cell(s->cursor_row,s->cursor_col,updated?updated->input.c_str():"",updated&&updated->literal)) find_next(find.value(),match.value());
            }
        }
    }
    grid->take_focus();
}
void ExcelWindow::template_book() {
    if (!confirm_discard()) return;
    Workbook next; if (!next.init()) { fl_alert("%s",T("Not enough memory")); return; }
    Sheet &ledger=next.sheets[0]; ledger.name.set(T("Ledger"));
    const char *headers[]={T("Date"),T("Category"),T("Description"),T("Income"),T("Expense"),T("Balance")};
    for (int col=0;col<6;++col) next.set_input(ledger.id,0,col,headers[col],true);
    const char *dates[]={"2026-10-01","2026-10-02","2026-10-03","2026-10-04","2026-10-05","2026-10-06","2026-10-07","2026-10-08"};
    const char *categories[]={T("Salary"),T("Food"),T("Transport"),T("Shopping"),T("Food"),T("Utilities"),T("Rent"),T("Food")};
    const char *descriptions[]={T("Monthly salary"),T("Lunch"),T("Transit pass"),T("Office supplies"),T("Groceries"),T("Electricity"),T("Monthly rent"),T("Dinner")};
    const char *expenses[]={"","35","100","299","156.8","89.5","1800","48"};
    for (int i=0;i<8;++i) {
        next.set_input(ledger.id,i+1,0,dates[i],true); next.set_input(ledger.id,i+1,1,categories[i],true); next.set_input(ledger.id,i+1,2,descriptions[i],true);
        if (!i) next.set_input(ledger.id,i+1,3,"8000");
        if (*expenses[i]) next.set_input(ledger.id,i+1,4,expenses[i]);
        char formula[96]; snprintf(formula,sizeof(formula),"=SUM($D$2:D%d)-SUM($E$2:E%d)",i+2,i+2); next.set_input(ledger.id,i+1,5,formula);
    }
    Style heading; heading.bold=1; heading.foreground=0xffffff; heading.background=0x217346; heading.size=15;
    Style money; money.format=NumberFormat::Currency; money.alignment=Alignment::Right;
    for (int col=0;col<6;++col) ledger.set_style(0,col,heading);
    for (int row=1;row<=8;++row) for (int col=0;col<6;++col) {
        Style style=col>=3?money:Style{}; if (!(row%2)) style.background=0xf0f6f1; ledger.set_style(row,col,style);
    }
    ledger.column_widths.resize(6); ledger.column_widths[0]=144; ledger.column_widths[1]=126; ledger.column_widths[2]=206;
    ledger.column_widths[3]=144; ledger.column_widths[4]=144; ledger.column_widths[5]=162;
    ledger.row_heights.resize(1); ledger.row_heights[0]=34; ledger.freeze_row=true;
    Text ledger_name=ledger.name; uint32_t ledger_id=ledger.id;
    Sheet summary; summary.id=next.next_id++; summary.name.set(T("Summary")); uint32_t summary_id=summary.id;
    if (!next.sheets.push(static_cast<Sheet &&>(summary))) return;
    const char *items[]={T("Item"),T("Total income"),T("Total expenses"),T("Remaining balance"),"",T("Food purchases"),T("Food expenses"),T("Average expense"),T("Budget status")};
    for (int row=0;row<9;++row) if (*items[row]) next.set_input(summary_id,row,0,items[row],true);
    next.set_input(summary_id,0,1,T("Amount"),true);
    char formula[512];
    snprintf(formula,sizeof(formula),"=SUM('%s'!D2:D9)",ledger_name.c_str()); next.set_input(summary_id,1,1,formula);
    snprintf(formula,sizeof(formula),"=SUM('%s'!E2:E9)",ledger_name.c_str()); next.set_input(summary_id,2,1,formula);
    next.set_input(summary_id,3,1,"=B2-B3");
    snprintf(formula,sizeof(formula),"=COUNTIF('%s'!B2:B9,\"%s\")",ledger_name.c_str(),T("Food")); next.set_input(summary_id,5,1,formula);
    snprintf(formula,sizeof(formula),"=SUMIF('%s'!B2:B9,\"%s\",'%s'!E2:E9)",ledger_name.c_str(),T("Food"),ledger_name.c_str()); next.set_input(summary_id,6,1,formula);
    snprintf(formula,sizeof(formula),"=AVERAGE('%s'!E3:E9)",ledger_name.c_str()); next.set_input(summary_id,7,1,formula);
    snprintf(formula,sizeof(formula),"=IF(B4>0,\"%s\",\"%s\")",T("Within budget"),T("Overspent")); next.set_input(summary_id,8,1,formula);
    Sheet *report=next.sheet(summary_id); report->column_widths.resize(2); report->column_widths[0]=240; report->column_widths[1]=220;
    for (int col=0;col<2;++col) report->set_style(0,col,heading);
    for (int row=1;row<9;++row) if (row!=5 && row!=8) report->set_style(row,1,money);
    Style total=money; total.bold=1; total.background=0xdceee0; report->set_style(3,1,total); report->freeze_row=true;
    next.active_id=ledger_id;
    if (!editor.replace_book(static_cast<Workbook &&>(next))) return;
    discard_recovery();
    path_.clear(); stamp_=FileStamp{}; saved_revision_=0; changed(true); grid->take_focus();
}
void ExcelWindow::act(Command command) {
    if (busy_ || !grid->finish_edit(true) || !commit_formula()) return;
    Sheet *s=sheet(); if (!s) return;
    uint32_t id=s->id; Range selected_range=grid->selection(); bool ok=true,mutated=false,lists=false;
    auto edit=[this](auto operation) { set_busy(true); bool result=operation(); set_busy(false); return result; };
    switch (command) {
    case Command::New: {
        if (!confirm_discard()) return;
        Workbook next; if (!next.init()) return; next.sheets[0].name.set(T("Sheet1"));
        if (!editor.replace_book(static_cast<Workbook &&>(next))) return;
        discard_recovery(); path_.clear(); stamp_=FileStamp{}; saved_revision_=editor.book.revision; changed(true); return;
    }
    case Command::Template: template_book(); return;
    case Command::Open: case Command::Import: {
        Text target; if (choose_path(target,false,command==Command::Import)) open(target.c_str(),command==Command::Import || has_suffix(target.c_str(),".csv")); return;
    }
    case Command::Save: save(false); return;
    case Command::SaveAs: save(true); return;
    case Command::Export: {
        if (fl_choice("%s",T("Cancel"),T("Export"),nullptr,T("CSV exports only this sheet's values. Formulas, styles and other sheets remain in the native workbook."))!=1) return;
        Text target,error; if (!choose_path(target,true,true)) return;
        FileStamp existing; if (!file_stamp(target.c_str(),existing,error)) { fl_alert("%s",T(error.c_str())); return; }
        if (existing.exists && fl_choice("%s",T("Cancel"),T("Replace"),nullptr,T("This file already exists. Replace it?"))!=1) return;
        set_busy(true); ok=export_csv(target.c_str(),editor.book,id,error,true,progress,this,&existing); set_busy(false);
        if (!ok) fl_alert("%s",T(error.c_str())); else message(T("CSV exported")); return;
    }
    case Command::Quit: close_cb(this,this); return;
    case Command::Undo: ok=edit([&]{ return editor.undo(); }); mutated=lists=ok; break;
    case Command::Redo: ok=edit([&]{ return editor.redo(); }); mutated=lists=ok; break;
    case Command::Copy: case Command::Cut: {
        set_busy(true); ok=editor.copy_range(id,selected_range,clipboard,command==Command::Cut);
        Text error; if (ok) ok=encode_delimited(editor.book,id,selected_range,clipboard_text,error,'\t'); set_busy(false);
        if (ok) { Fl::copy(clipboard_text.c_str(),int(clipboard_text.size()),1); clipboard_owner=selection_owner(); message(command==Command::Cut?T("Select a destination and paste to move the cells"):T("Cells copied")); }
        else fl_alert("%s",T(error.empty()?editor.error.c_str():error.c_str()));
        return;
    }
    case Command::Paste: case Command::PasteValues:
        paste_values_=command==Command::PasteValues; grid->take_focus(); Fl::paste(*grid,1); return;
    case Command::SelectAll: grid->select_range(data_range(s,Range{})); grid->take_focus(); return;
    case Command::FillDown: case Command::FillRight: {
        Range source=selected_range;
        if (command==Command::FillDown) { if (source.r1==source.r2) { message(T("Select at least two rows to fill down")); return; } source.r2=source.r1; }
        else { if (source.c1==source.c2) { message(T("Select at least two columns to fill right")); return; } source.c2=source.c1; }
        set_busy(true); ok=editor.fill(id,source,selected_range); set_busy(false); mutated=ok; break;
    }
    case Command::Bold: case Command::Italic: case Command::Size: case Command::TextColor: case Command::FillColor:
    case Command::Borders: case Command::NoBorders: case Command::AlignLeft: case Command::AlignCenter: case Command::AlignRight:
    case Command::Wrap: case Command::General: case Command::Number: case Command::Percent: case Command::Currency: {
        const Cell *cell=s->get(s->cursor_row,s->cursor_col); Style style=cell?cell->style:Style{}; uint32_t mask=0;
        if (command==Command::Bold) { style.bold=!style.bold; mask=8; }
        else if (command==Command::Italic) { style.italic=!style.italic; mask=16; }
        else if (command==Command::Wrap) { style.wrap=!style.wrap; mask=32; }
        else if (command==Command::Size) { const int sizes[]={10,12,14,16,18,20,24,28,32,36}; style.size=sizes[clamp(size_->value(),0,9)]; mask=4; }
        else if (command==Command::Borders || command==Command::NoBorders) { style.borders=command==Command::Borders?15:0; mask=64; }
        else if (command==Command::AlignLeft || command==Command::AlignCenter || command==Command::AlignRight) { style.alignment=command==Command::AlignLeft?Alignment::Left:command==Command::AlignCenter?Alignment::Center:Alignment::Right; mask=128; }
        else if (command==Command::TextColor || command==Command::FillColor) {
            uint32_t rgb=command==Command::TextColor?style.foreground:style.background;
            double red=(rgb>>16)/255.,green=((rgb>>8)&255)/255.,blue=(rgb&255)/255.;
            if (!fl_color_chooser(command==Command::TextColor?T("Text color"):T("Fill color"),red,green,blue)) return;
            rgb=(uint32_t(red*255+0.5)<<16)|(uint32_t(green*255+0.5)<<8)|uint32_t(blue*255+0.5);
            if (command==Command::TextColor) { style.foreground=rgb; mask=1; } else { style.background=rgb; mask=2; }
        } else { style.format=command==Command::General?NumberFormat::General:command==Command::Number?NumberFormat::Number:command==Command::Percent?NumberFormat::Percent:NumberFormat::Currency; mask=256; }
        set_busy(true); ok=editor.patch_style(id,selected_range,style,mask); set_busy(false); mutated=ok; break;
    }
    case Command::InsertRow: case Command::DeleteRow: case Command::InsertColumn: case Command::DeleteColumn: {
        bool rows=command==Command::InsertRow || command==Command::DeleteRow;
        bool remove=command==Command::DeleteRow || command==Command::DeleteColumn;
        int first=rows?selected_range.r1:selected_range.c1,count=rows?selected_range.r2-first+1:selected_range.c2-first+1;
        set_busy(true); ok=remove?editor.delete_axis(id,rows,first,count):editor.insert_axis(id,rows,first,count); set_busy(false); mutated=lists=ok; break;
    }
    case Command::Merge: case Command::Unmerge: {
        bool discard=false;
        if (command==Command::Merge) {
            unsigned populated=0; for (size_t i=0;i<s->cells.size();++i) if (selected_range.contains(s->cells[i].row,s->cells[i].col) && !s->cells[i].input.empty()) ++populated;
            if (populated>1) { if (fl_choice("%s",T("Cancel"),T("Merge"),nullptr,T("Only the upper-left cell's content is kept when merging. Continue?"))!=1) return; discard=true; }
        }
        set_busy(true); ok=editor.merge(id,selected_range,command==Command::Unmerge,discard); set_busy(false); mutated=ok; break;
    }
    case Command::AddSheet: {
        char name[96]; unsigned index=1;
        do { snprintf(name,sizeof(name),T("Sheet%u"),index++); } while (editor.book.find_sheet(name));
        ok=edit([&]{ return editor.add_sheet(name); }); mutated=lists=ok; break;
    }
    case Command::DeleteSheet:
        if (fl_choice("%s",T("Cancel"),T("Delete sheet"),nullptr,T("Delete this worksheet? You can undo this operation."))!=1) return;
        ok=edit([&]{ return editor.delete_sheet(id); }); mutated=lists=ok; break;
    case Command::RenameSheet: {
        const char *name=fl_input("%s",s->name.c_str(),T("Worksheet name:")); if (!name) return;
        ok=edit([&]{ return editor.rename_sheet(id,name); }); mutated=lists=ok; break;
    }
    case Command::MoveLeft: case Command::MoveRight:
        ok=edit([&]{ return editor.move_sheet(id,command==Command::MoveLeft?-1:1); }); mutated=lists=ok; break;
    case Command::AutoSum: {
        Range range=selected_range; int row=s->cursor_row,col=s->cursor_col;
        if (range.r2>range.r1 || range.c2>range.c1) { row=range.r2+1; col=range.c1; }
        else if (row>0) { range={0,col,row-1,col}; }
        else if (col>0) range={row,0,row,col-1};
        else { message(T("Select numbers or a cell below the numbers to sum")); return; }
        if (row>=MaxRows) { fl_alert("%s",T("There is no room below this range")); return; }
        const Cell *target=s->get(row,col);
        if (target && !target->input.empty() && fl_choice("%s",T("Cancel"),T("Replace"),nullptr,T("The destination contains data. Replace it?"))!=1) return;
        char first[12],last[12],formula[96]; column_label(range.c1,first,sizeof(first)); column_label(range.c2,last,sizeof(last));
        snprintf(formula,sizeof(formula),"=SUM(%s%d:%s%d)",first,range.r1+1,last,range.r2+1);
        ok=commit_cell(row,col,formula); if (ok) grid->select(row,col); return;
    }
    case Command::Function: function_dialog(); return;
    case Command::Recalculate: editor.book.invalidate(); changed(); return;
    case Command::Formulas: show_formulas=!show_formulas; grid->redraw(); selected(); return;
    case Command::SortAscending: data_dialog(false,false); return;
    case Command::SortDescending: data_dialog(false,true); return;
    case Command::Filter: data_dialog(true,false); return;
    case Command::ClearFilter: ok=edit([&]{ return editor.clear_filter(id); }); mutated=ok; break;
    case Command::Find: search_dialog(false); return;
    case Command::Replace: search_dialog(true); return;
    case Command::FreezeRow: case Command::FreezeColumn: case Command::FreezeBoth: case Command::Unfreeze: case Command::Gridlines: case Command::Zoom: {
        bool row=s->freeze_row,col=s->freeze_column,lines=s->gridlines; int zoom=s->zoom;
        if (command==Command::FreezeRow) row=true;
        else if (command==Command::FreezeColumn) col=true;
        else if (command==Command::FreezeBoth) row=col=true;
        else if (command==Command::Unfreeze) row=col=false;
        else if (command==Command::Gridlines) lines=!lines;
        else {
            char initial[16]; snprintf(initial,sizeof(initial),"%d",zoom);
            const char *value=fl_input("%s",initial,T("Zoom percentage (25–400):")); if (!value) return;
            char *end=nullptr; long n=strtol(value,&end,10); if (!*value || *end || n<25 || n>400) { fl_alert("%s",T("Enter a zoom value from 25 to 400")); return; } zoom=int(n);
        }
        ok=edit([&]{ return editor.set_view(id,row,col,lines,zoom); }); mutated=ok; break;
    }
    }
    if (mutated) changed(lists);
    else if (!ok && !editor.error.empty()) fl_alert("%s",T(editor.error.c_str()));
    grid->take_focus();
}
void ExcelWindow::recovery_tick(void *context) {
    ExcelWindow *w=static_cast<ExcelWindow *>(context);
    if (!w->busy_ && !w->grid->editing() && w->editor.book.revision!=w->saved_revision_ && w->editor.book.revision!=w->recovery_revision_) {
        Text error;
        bool reserved=!w->recovery_path_.empty();
        if (!reserved && reserve_recovery_path(w->recovery_path_,error)) {
            reserved=lock_recovery_path(w->recovery_path_.c_str(),w->recovery_descriptor_,error);
            if (!reserved) { unlink(w->recovery_path_.c_str()); w->recovery_path_.clear(); }
        }
        if (reserved) {
            w->set_busy(true); bool ok=save_workbook(w->recovery_path_.c_str(),w->editor.book,error,nullptr,progress,w); w->set_busy(false);
            if (ok) { w->recovery_revision_=w->editor.book.revision; w->message(T("Recovery copy saved")); }
            else w->message(T("Could not save the recovery copy"));
        } else w->message(T("Could not save the recovery copy"));
    }
    Fl::repeat_timeout(60,recovery_tick,w);
}
void ExcelWindow::offer_recovery() {
    Text directory,error,candidate; if (!recovery_directory(directory,error)) return;
    DIR *dir=opendir(directory.c_str()); if (!dir) return;
    int64_t newest=0; int descriptor=-1; struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (!has_suffix(entry->d_name,".rexl")) continue;
        char path[4096]; snprintf(path,sizeof(path),"%s/%s",directory.c_str(),entry->d_name);
        struct stat status;
        if (!lstat(path,&status) && S_ISREG(status.st_mode) && status.st_uid==getuid() && status.st_size>=16 && status.st_mtime>=newest) {
            int next=-1;
            if (!lock_recovery_path(path,next,error)) continue;
            if (!candidate.set(path)) { unlock_recovery_path(next); continue; }
            unlock_recovery_path(descriptor); descriptor=next; newest=status.st_mtime;
        }
    }
    closedir(dir); if (candidate.empty()) return;
    int result=fl_choice("%s",T("Later"),T("Recover"),T("Discard"),T("An unfinished workbook was found. Recover it?"));
    if (result==2) {
        remove_recovery_copy(candidate,descriptor); return;
    }
    if (result!=1) { unlock_recovery_path(descriptor); return; }
    Workbook recovered; set_busy(true); bool ok=load_workbook(candidate.c_str(),recovered,error,progress,this); set_busy(false);
    if (!ok) { unlock_recovery_path(descriptor); fl_alert("%s",T(error.c_str())); return; }
    editor.replace_book(static_cast<Workbook &&>(recovered)); recovered_from_=candidate; recovered_descriptor_=descriptor; path_.clear(); saved_revision_=0; changed(true);
}
void ExcelWindow::resize(int X,int Y,int W,int H) {
    Fl_Double_Window::resize(X,Y,W,H);
    if (!grid) return;
    ribbon_tabs_->resize(0,0,W,34); ribbon_->resize(0,34,W,100); ribbon_scroll_->resize(0,34,W,100);
    formula_group_->resize(0,136,W,34); name_->resize(8,139,106,28); formula_->resize(164,139,W-172,28);
    grid->resize(0,172,W,H-232); bottom_->resize(0,H-60,W,60); sheet_scroll_->resize(0,H-60,W,34);
    status_->resize(8,H-24,W/2-12,22); statistics_->resize(W/2,H-24,W/2-8,22); cancel_->resize(W-88,H-24,80,22);
}
int ExcelWindow::handle(int event) {
    if ((event==FL_KEYDOWN || event==FL_SHORTCUT) && !busy_ && Fl::focus()==formula_ && !Fl::compose_state) {
        if (Fl::event_key()==FL_Escape) { selected(); grid->take_focus(); return 1; }
        if (Fl::event_key()==FL_Tab) {
            if (commit_formula()) { grid->take_focus(); grid->move(0,Fl::event_state(FL_SHIFT)?-1:1); }
            return 1;
        }
    }
    if (event==FL_PUSH && !busy_) {
        if (grid && grid->editing() && !Fl::event_inside(grid) && !grid->finish_edit(true)) return 1;
        if (formula_ && Fl::focus()==formula_ && !Fl::event_inside(formula_) && !commit_formula()) return 1;
    }
    if ((event==FL_KEYDOWN || event==FL_SHORTCUT) && Fl::event_state(FL_CTRL) && !Fl::compose_state) {
        int key=Fl::event_key(); bool shift=Fl::event_state(FL_SHIFT);
        if (!busy_ && (key=='c' || key=='x') && (grid->editing() || Fl::focus()==formula_ || Fl::focus()==name_)) {
            clipboard_owner=0; clipboard.cut=false;
        }
        if (key=='s') { act(shift?Command::SaveAs:Command::Save); return 1; }
        if (key=='o') { act(Command::Open); return 1; }
        if (key=='n') { act(Command::New); return 1; }
        if (key=='f') { act(Command::Find); return 1; }
        if (key=='h') { act(Command::Replace); return 1; }
        if (key=='z' && !grid->editing() && Fl::focus()!=formula_) { act(shift?Command::Redo:Command::Undo); return 1; }
        if (key=='y' && !grid->editing() && Fl::focus()!=formula_) { act(Command::Redo); return 1; }
        if (key=='b' && !grid->editing() && Fl::focus()!=formula_) { act(Command::Bold); return 1; }
        if (key=='i' && !grid->editing() && Fl::focus()!=formula_) { act(Command::Italic); return 1; }
    }
    return Fl_Double_Window::handle(event);
}
}
