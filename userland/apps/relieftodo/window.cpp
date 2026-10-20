#include "window.h"
#include <FL/fl_draw.H>
#include <FL/fl_ask.H>
#include <stdio.h>
#include <time.h>
namespace rt {
namespace {
const char *priorities[]={"Low","Normal","High","Urgent"};
const char *filter_names[]={"All tasks","Today","Upcoming","Completed","Overdue","Active tasks"};
const Filter filter_order[]={Filter::All,Filter::Active,Filter::Today,Filter::Upcoming,Filter::Overdue,Filter::Completed};
class NotesEditor : public Fl_Text_Editor {
    TodoWindow *owner_;
public:
    NotesEditor(int x,int y,int w,int h,TodoWindow *owner):Fl_Text_Editor(x,y,w,h),owner_(owner) {}
    int handle(int event) override {
        if ((event==FL_KEYDOWN || event==FL_SHORTCUT) && (Fl::event_state()&FL_CTRL) &&
            (Fl::event_key()==FL_Enter || Fl::event_key()==FL_KP_Enter)) {
            owner_->run(Command::Apply);
            return 1;
        }
        return Fl_Text_Editor::handle(event);
    }
};
bool has(const Array<TaskId> &ids,TaskId id) {
    for (size_t n=0;n<ids.size();++n) if (ids[n]==id) return true;
    return false;
}
void label_style(Fl_Box *box) { box->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); box->labelsize(13); }
void ribbon_label(int x,int width,const char *label) { auto *b=new Fl_Box(x,104,width,18,label); b->labelsize(12); }
}
TaskTable::TaskTable(int x,int y,int w,int h,TodoWindow *owner):Fl_Table_Row(x,y,w,h),owner_(owner) {
    cols(5); col_header(1); col_header_height(31); row_header(0); row_height_all(36);
    col_resize(1); col_resize_min(48); type(SELECT_MULTI); end(); columns();
    when(FL_WHEN_RELEASE); tooltip(T("Ctrl or Shift selects multiple tasks; double-click toggles completion"));
}
void TaskTable::columns() {
    int fixed=66+66+104+96+20;
    col_width(0,w()-fixed>190?w()-fixed:190); col_width(1,66); col_width(2,66); col_width(3,104); col_width(4,96);
}
void TaskTable::draw_cell(TableContext context,int row,int col,int x,int y,int width,int height) {
    if (context==CONTEXT_STARTPAGE) { fl_font(FL_HELVETICA,14); return; }
    if (context==CONTEXT_COL_HEADER) {
        const char *names[]={T("Task title"),T("Status"),T("Priority"),T("Due date"),T("Project")};
        fl_draw_box(FL_THIN_UP_BOX,x,y,width,height,FL_BACKGROUND_COLOR);
        fl_color(FL_FOREGROUND_COLOR); fl_draw(names[col],x+8,y,width-16,height,FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); return;
    }
    if (context!=CONTEXT_CELL || row<0 || size_t(row)>=owner_->visible_.size()) return;
    const Task &t=owner_->document_.tasks[owner_->visible_[size_t(row)]];
    bool selected=row_selected(row)!=0;
    Fl_Color background=selected?selection_color():FL_BACKGROUND2_COLOR;
    fl_push_clip(x,y,width,height); fl_color(background); fl_rectf(x,y,width,height);
    fl_color(fl_contrast(FL_FOREGROUND_COLOR,background));
    const char *text=col==0?t.title.c_str():col==1?(t.completed?T("Done"):T("To do")):
        col==2?T(priorities[unsigned(t.priority)]):col==3?(t.due.empty()?T("No date"):t.due.c_str()):t.project.c_str();
    fl_font(FL_HELVETICA,14); fl_draw(text,x+8,y,width-16,height,FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);
    fl_color(fl_color_average(FL_FOREGROUND_COLOR,background,.12f)); fl_line(x,y+height-1,x+width,y+height-1); fl_pop_clip();
}
int TaskTable::handle(int event) {
    if (event==FL_PUSH && !owner_->apply_editor(false)) return 1;
    if (event==FL_KEYDOWN && Fl::event_key()==FL_Down) { owner_->select_relative(1); return 1; }
    if (event==FL_KEYDOWN && Fl::event_key()==FL_Up) { owner_->select_relative(-1); return 1; }
    if (event==FL_KEYDOWN && Fl::event_key()==' ') { owner_->run(Command::Toggle); return 1; }
    if (event==FL_KEYDOWN && Fl::event_key()==FL_Delete) { owner_->run(Command::Delete); return 1; }
    int result=Fl_Table_Row::handle(event);
    if (event==FL_RELEASE && callback_context()==CONTEXT_CELL) {
        owner_->table_selection(callback_row());
        if (Fl::event_clicks()) owner_->run(Command::Toggle);
    }
    return result;
}
TodoWindow::TodoWindow():Fl_Double_Window(1180,760,"ReliefTodo") {
    size_range(1000,650); callback([](Fl_Widget *,void *p){ static_cast<TodoWindow *>(p)->run(Command::Close); },this);
    begin(); top_=new Fl_Group(0,0,w(),34);
    auto *file=new Fl_Menu_Button(8,3,70,28,T("&File"));
    auto menu=[file](const char *label,Command c,int key=0) { file->add(label,key,clicked,reinterpret_cast<void *>(intptr_t(c))); };
    menu(T("New task list"),Command::New,FL_CTRL+'n'); menu(T("Open..."),Command::Open,FL_CTRL+'o');
    menu(T("Save"),Command::Save,FL_CTRL+'s'); menu(T("Save as..."),Command::SaveAs,FL_CTRL+FL_SHIFT+'s');
    menu(T("Export CSV..."),Command::ExportCsv); menu(T("Export Markdown..."),Command::ExportMarkdown);
    menu(T("Example task list"),Command::Example); menu(T("Task statistics"),Command::Statistics);
    menu(T("ReliefTodo help"),Command::Help,FL_F+1); menu(T("Exit"),Command::Close);
    const char *names[]={T("Home tab"),T("Task tools"),T("View")};
    for (int n=0;n<3;++n) {
        tabs_[n]=new Fl_Button(86+86*n,3,80,28,names[n]); tabs_[n]->type(FL_TOGGLE_BUTTON);
        tabs_[n]->callback([](Fl_Widget *w,void *p){ static_cast<TodoWindow *>(w->window())->show_tab(int(intptr_t(p))); },reinterpret_cast<void *>(intptr_t(n)));
    }
    const char *quick[]={T("Save"),T("Undo"),T("Redo")}; const Command actions[]={Command::Save,Command::Undo,Command::Redo};
    for (int n=0;n<3;++n) {
        auto *b=new Fl_Button(w()-222+72*n,4,66,26,quick[n]); b->callback(clicked,reinterpret_cast<void *>(intptr_t(actions[n])));
        if (n==1) undo_=b; if (n==2) redo_=b;
    }
    top_->end(); ribbon_=new Fl_Group(0,34,w(),100);
    ribbon_scroll_=new Fl_Scroll(0,34,w(),100); ribbon_scroll_->type(Fl_Scroll::HORIZONTAL); ribbon_scroll_->scrollbar_size(10);
    build_ribbon(); ribbon_scroll_->end(); ribbon_->end();
    body_=new Fl_Group(0,140,w(),h()-174);
    left_=new Fl_Group(8,144,180,h()-182);
    auto *find_label=new Fl_Box(10,146,176,20,T("Search tasks")); label_style(find_label);
    search_=new Fl_Input(10,170,176,29); search_->when(FL_WHEN_CHANGED);
    search_->tooltip(T("Search titles, notes, projects and tags"));
    search_->callback([](Fl_Widget *w,void *) {
        auto *self=static_cast<TodoWindow *>(w->window()); Fl::remove_timeout(search_tick,self); Fl::add_timeout(.18,search_tick,self);
    });
    filters_=new Fl_Hold_Browser(10,211,176,205); filters_->textsize(14); filters_->format_char(0);
    filters_->callback([](Fl_Widget *w,void *){ static_cast<TodoWindow *>(w->window())->choose_filter(); });
    auto *projects_label=new Fl_Box(10,428,176,23,T("Projects")); label_style(projects_label);
    project_list_=new Fl_Hold_Browser(10,455,176,h()-499); project_list_->textsize(14); project_list_->format_char(0);
    project_list_->callback([](Fl_Widget *w,void *){ static_cast<TodoWindow *>(w->window())->choose_project(); });
    left_->end(); center_=new Fl_Group(198,144,w()-532,h()-182);
    list_heading_=new Fl_Box(200,146,w()-720,29,T("All tasks")); label_style(list_heading_); list_heading_->labelsize(16);
    sort_choice_=new Fl_Choice(w()-512,146,176,29);
    const char *sort_names[]={"Manual order","Priority","Due date","Recently updated","Task title"};
    for (const char *s:sort_names) sort_choice_->add(T(s));
    sort_choice_->value(0); sort_choice_->tooltip(T("Sort tasks"));
    sort_choice_->callback([](Fl_Widget *w,void *){ static_cast<TodoWindow *>(w->window())->choose_sort(); });
    table_=new TaskTable(200,184,w()-536,h()-232,this);
    center_->end(); details_=new Fl_Group(w()-326,144,318,h()-182);
    detail_heading_=new Fl_Box(w()-318,146,300,29,T("Task details")); label_style(detail_heading_); detail_heading_->labelsize(16);
    const char *labels[]={T("Task title"),T("Project"),T("Priority"),T("Due date (YYYY-MM-DD)"),T("Tags (comma separated)"),T("Notes"),T("Use Ctrl+Enter to apply changes")};
    for (int n=0;n<7;++n) { labels_[n]=new Fl_Box(w()-318,183,300,18,labels[n]); label_style(labels_[n]); }
    title_=new Fl_Input(w()-318,204,300,29); title_->maximum_size(int(MaxTitle));
    project_input_=new Fl_Input(w()-318,259,176,28); project_input_->maximum_size(int(MaxProject));
    priority_=new Fl_Choice(w()-132,259,114,28); for (const char *s:priorities) priority_->add(T(s));
    due_=new Fl_Input(w()-318,314,300,28); due_->maximum_size(10);
    tags_=new Fl_Input(w()-318,403,300,28); tags_->maximum_size(int(MaxTags));
    const char *dates[]={T("Today"),T("Tomorrow"),T("Clear date")}; const Command date_actions[]={Command::Today,Command::Tomorrow,Command::ClearDate};
    for (int n=0;n<3;++n) { date_buttons_[n]=new Fl_Button(w()-318+n*102,350,96,26,dates[n]); date_buttons_[n]->callback(clicked,reinterpret_cast<void *>(intptr_t(date_actions[n]))); }
    completed_=new Fl_Check_Button(w()-318,440,300,28,T("Task completed"));
    notes_editor_=new NotesEditor(w()-318,493,300,h()-590,this); notes_editor_->buffer(&notes_); notes_editor_->textsize(15);
    notes_editor_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS,0); notes_.add_modify_callback(notes_changed,this);
    apply_=new Fl_Button(w()-318,h()-89,146,30,T("Apply changes")); apply_->callback(clicked,reinterpret_cast<void *>(intptr_t(Command::Apply)));
    revert_=new Fl_Button(w()-164,h()-89,146,30,T("Revert changes")); revert_->callback(clicked,reinterpret_cast<void *>(intptr_t(Command::Revert)));
    meta_=new Fl_Box(w()-318,h()-52,300,18); label_style(meta_); meta_->labelsize(12);
    Fl_Widget *fields[]={title_,project_input_,due_,tags_,priority_,completed_};
    for (Fl_Widget *field:fields) { field->when(FL_WHEN_CHANGED); field->callback(edited,this); }
    details_->end(); body_->end(); bottom_=new Fl_Group(0,h()-28,w(),28);
    status_=new Fl_Box(8,h()-27,w()/2-16,26,T("Ready")); label_style(status_);
    summary_=new Fl_Box(w()/2,h()-27,w()/2-8,26); summary_->align(FL_ALIGN_RIGHT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP); summary_->labelsize(13);
    bottom_->end(); end(); resizable(body_);
    Fl_Group *groups[]={top_,ribbon_,body_,left_,center_,details_,bottom_,pages_[0],pages_[1],pages_[2]};
    for (Fl_Group *group:groups) group->resizable(nullptr);
    seed_example(document_); saved_.clone_from(document_);
    if (document_.tasks.size()) { active_=document_.tasks[0].id; selected_.push(active_); }
    show_tab(0); layout(); refill(); refresh_editor();
    Fl::add_timeout(.03,tick,this);
    int x,y,width,height; Fl::screen_work_area(x,y,width,height);
    int ww=w()<width-24?w():width-24, hh=h()<height-44?h():height-44;
    if (ww>=1000 && hh>=650) resize(x+(width-ww)/2,y+(height-hh)/2,ww,hh);
}
TodoWindow::~TodoWindow() {
    Fl::remove_timeout(tick,this); Fl::remove_timeout(search_tick,this);
    notes_.remove_modify_callback(notes_changed,this); notes_editor_->buffer(nullptr);
}
void TodoWindow::clicked(Fl_Widget *w,void *data) { static_cast<TodoWindow *>(w->window())->run(Command(intptr_t(data))); }
void TodoWindow::edited(Fl_Widget *,void *data) {
    auto *self=static_cast<TodoWindow *>(data); if (self->loading_) return;
    self->fields_dirty_=true; self->apply_->activate(); self->revert_->activate(); self->update_status(T("Task details have unapplied changes"));
}
void TodoWindow::notes_changed(int,int inserted,int removed,int,const char *,void *p) { if (inserted || removed) edited(nullptr,p); }
void TodoWindow::build_ribbon() {
    auto b=[](int x,int y,int width,const char *label,Command c,int height=26) {
        auto *button=new Fl_Button(x,y,width,height,label); button->callback(clicked,reinterpret_cast<void *>(intptr_t(c))); button->tooltip(label); return button;
    };
    pages_[0]=new Fl_Group(0,34,1050,90);
    b(10,42,80,T("New task"),Command::Add,54);
    b(98,42,84,T("Duplicate"),Command::Duplicate); b(98,72,84,T("Delete"),Command::Delete); ribbon_label(10,172,T("Tasks"));
    b(202,42,104,T("Mark done"),Command::Done); b(202,72,104,T("Restore task"),Command::Restore); ribbon_label(202,104,T("Completion"));
    b(326,42,88,T("Today"),Command::Today); b(420,42,88,T("Tomorrow"),Command::Tomorrow);
    b(326,72,88,T("Next week"),Command::NextWeek); b(420,72,88,T("Clear date"),Command::ClearDate); ribbon_label(326,182,T("Schedule"));
    b(528,42,86,T("Move up"),Command::MoveUp); b(528,72,86,T("Move down"),Command::MoveDown); ribbon_label(528,86,T("Manual order"));
    b(634,42,110,T("Apply changes"),Command::Apply); b(634,72,110,T("Revert changes"),Command::Revert); ribbon_label(634,110,T("Editing"));
    b(764,42,104,T("Select all"),Command::SelectAll); b(764,72,104,T("Clear selection"),Command::ClearSelection); ribbon_label(764,104,T("Selection"));
    b(888,42,140,T("Task statistics"),Command::Statistics); b(888,72,140,T("ReliefTodo help"),Command::Help); ribbon_label(888,140,T("Tools")); pages_[0]->end();
    pages_[1]=new Fl_Group(0,34,1010,90);
    b(10,42,88,T("Urgent"),Command::Urgent); b(104,42,88,T("High"),Command::High);
    b(10,72,88,T("Normal"),Command::Normal); b(104,72,88,T("Low"),Command::Low); ribbon_label(10,182,T("Priority"));
    b(214,42,118,T("Set project..."),Command::Project); b(214,72,118,T("Set tags..."),Command::Tags); ribbon_label(214,118,T("Organize"));
    b(354,42,126,T("Rename task list"),Command::Rename); b(354,72,126,T("Task statistics"),Command::Statistics); ribbon_label(354,126,T("Task list"));
    b(502,42,152,T("Export CSV..."),Command::ExportCsv); b(502,72,152,T("Export Markdown..."),Command::ExportMarkdown); ribbon_label(502,152,T("Export"));
    b(676,42,144,T("Example task list"),Command::Example); b(676,72,144,T("ReliefTodo help"),Command::Help); ribbon_label(676,144,T("Learn")); pages_[1]->end();
    pages_[2]=new Fl_Group(0,34,1010,90);
    b(10,42,134,T("Clear all filters"),Command::ClearFilters,54); ribbon_label(10,134,T("Filters"));
    b(166,42,130,T("Select all"),Command::SelectAll); b(166,72,130,T("Clear selection"),Command::ClearSelection); ribbon_label(166,130,T("Visible tasks"));
    b(318,42,144,T("Task statistics"),Command::Statistics); b(318,72,144,T("Rename task list"),Command::Rename); ribbon_label(318,144,T("Task list")); pages_[2]->end();
}
void TodoWindow::show_tab(int tab) {
    tab_=tab;
    for (int n=0;n<3;++n) { tabs_[n]->value(n==tab); if (n==tab) pages_[n]->show(); else pages_[n]->hide(); }
    ribbon_scroll_->scroll_to(0,0); ribbon_scroll_->redraw();
}
void TodoWindow::layout() {
    top_->resize(0,0,w(),34);
    for (int n=0;n<3;++n) top_->child(top_->children()-3+n)->resize(w()-222+72*n,4,66,26);
    ribbon_->resize(0,34,w(),100); ribbon_scroll_->resize(0,34,w(),100);
    // Explicit geometry keeps fixed sidebars and distributes extra width to
    // the task table. It never scales button text or changes FLTK box styles.
    body_->resize(0,140,w(),h()-174); left_->resize(8,144,180,h()-182);
    project_list_->resize(10,455,176,h()-499);
    center_->resize(198,144,w()-532,h()-182);
    list_heading_->resize(200,146,w()-720,29); sort_choice_->resize(w()-512,146,176,29);
    table_->resize(200,184,w()-536,h()-232); table_->columns();
    const int x=w()-318;
    details_->resize(w()-326,144,318,h()-182); detail_heading_->resize(x,146,300,29);
    labels_[0]->resize(x,183,300,18); title_->resize(x,204,300,29);
    labels_[1]->resize(x,240,176,18); project_input_->resize(x,261,176,28);
    labels_[2]->resize(x+186,240,114,18); priority_->resize(x+186,261,114,28);
    labels_[3]->resize(x,295,300,18); due_->resize(x,316,300,28);
    for (int n=0;n<3;++n) date_buttons_[n]->resize(x+102*n,352,96,26);
    labels_[4]->resize(x,386,300,18); tags_->resize(x,407,300,28); completed_->resize(x,442,300,28);
    labels_[5]->resize(x,475,300,18); notes_editor_->resize(x,497,300,h()-594);
    apply_->resize(x,h()-89,146,30); revert_->resize(x+154,h()-89,146,30);
    labels_[6]->hide(); meta_->resize(x,h()-52,300,18);
    bottom_->resize(0,h()-28,w(),28); status_->resize(8,h()-27,w()/2-16,26); summary_->resize(w()/2,h()-27,w()/2-8,26);
}
void TodoWindow::resize(int x,int y,int width,int height) { Fl_Double_Window::resize(x,y,width,height); if (table_) layout(); }
bool TodoWindow::dirty() const { return force_dirty_ || !same_document_content(document_,saved_); }
void TodoWindow::update_status(const char *message) {
    if (message) status_->copy_label(message);
    size_t done=0; for (size_t n=0;n<document_.tasks.size();++n) if (document_.tasks[n].completed) ++done;
    char text[1024]; snprintf(text,sizeof(text),T("%zu tasks · %zu active · %zu done · %zu selected"),document_.tasks.size(),document_.tasks.size()-done,done,selected_.size()); summary_->copy_label(text);
    const char *name=path_.empty()?document_.title.c_str():strrchr(path_.c_str(),'/');
    if (!name) name=path_.c_str(); else if (*name=='/') ++name;
    snprintf(text,sizeof(text),"%s%s — ReliefTodo",(dirty()||fields_dirty_)?"* ":"",name); copy_label(text);
    if (history_.can_undo() && !io_.busy()) undo_->activate(); else undo_->deactivate();
    if (history_.can_redo() && !io_.busy()) redo_->activate(); else redo_->deactivate();
}
void TodoWindow::refill(bool projects) {
    char today[11]{}; local_date(today); visible_.clear(); visible_.reserve(document_.tasks.size());
    for (size_t n=0;n<document_.tasks.size();++n) if (task_matches(document_.tasks[n],filter_,query_.c_str(),today,project_.empty()?nullptr:project_.c_str())) visible_.push(n);
    table_->rows(int(visible_.size())); table_->row_height_all(36); table_->select_all_rows(0);
    Array<TaskId> kept;
    for (size_t n=0;n<visible_.size();++n) if (has(selected_,document_.tasks[visible_[n]].id)) { kept.push(document_.tasks[visible_[n]].id); table_->select_row(int(n),1); }
    selected_.swap(kept);
    if (!has(selected_,active_)) active_=selected_.empty()?0:selected_[0];
    if (!active_ && visible_.size()) { active_=document_.tasks[visible_[0]].id; selected_.push(active_); table_->select_row(0,1); }
    filters_->clear();
    for (int n=0;n<6;++n) {
        size_t count=0;
        for (size_t k=0;k<document_.tasks.size();++k) if (task_matches(document_.tasks[k],filter_order[n],"",today)) ++count;
        char label[128]; snprintf(label,sizeof(label),"%s  (%zu)",T(filter_names[unsigned(filter_order[n])]),count); filters_->add(label);
        if (filter_order[n]==filter_) filters_->value(n+1);
    }
    if (projects) {
        projects_.clear(); project_list_->clear(); project_list_->add(T("All projects"));
        for (size_t n=0;n<document_.tasks.size();++n) {
            const Text &p=document_.tasks[n].project; if (p.empty()) continue;
            bool seen=false; for (size_t k=0;k<projects_.size();++k) if (projects_[k]==p) seen=true;
            if (!seen) { projects_.push(p); project_list_->add(p.c_str()); }
        }
        int choice=1;
        for (size_t n=0;n<projects_.size();++n) if (project_==projects_[n]) choice=int(n)+2;
        project_list_->value(choice);
    }
    char heading[256]; snprintf(heading,sizeof(heading),"%s · %zu",T(filter_names[unsigned(filter_)]),visible_.size()); list_heading_->copy_label(heading);
    sort_choice_->value(int(sort_)); table_->redraw(); update_status();
}
void TodoWindow::refresh_editor() {
    loading_=true; const Task *t=find_task(document_,active_);
    title_->value(t?t->title.c_str():""); project_input_->value(t?t->project.c_str():""); due_->value(t?t->due.c_str():"");
    tags_->value(t?t->tags.c_str():""); priority_->value(t?int(t->priority):1); completed_->value(t&&t->completed?1:0); notes_.text(t?t->notes.c_str():"");
    const char *heading=selected_.size()>1?T("Details of the active task"):T("Task details"); detail_heading_->copy_label(heading);
    char text[128]; snprintf(text,sizeof(text),t?T("Task #%llu · Ctrl+Enter to apply"):T("Select a task to edit details"),static_cast<unsigned long long>(active_)); meta_->copy_label(text);
    for (int n=0;n<details_->children();++n) { Fl_Widget *child=details_->child(n); if (t && !io_.busy()) child->activate(); else child->deactivate(); }
    apply_->deactivate(); revert_->deactivate(); fields_dirty_=false; loading_=false; update_status();
}
bool TodoWindow::begin_change() {
    if (!history_.checkpoint(document_)) { update_status(T("Not enough memory for undo history")); return false; }
    return true;
}
void TodoWindow::finish_change(bool ok,const char *message) {
    if (ok) history_.committed(); else history_.rollback(document_);
    refill(); refresh_editor(); update_status(ok?message:T("Operation failed: check task limits or memory"));
}
bool TodoWindow::apply_editor(bool refresh) {
    if (loading_ || !fields_dirty_) return true;
    if (io_.busy()) return false;
    if (!valid_date(due_->value())) { update_status(T("Enter a valid date as YYYY-MM-DD, or leave it empty")); due_->take_focus(); return false; }
    char *notes=notes_.text();
    if (!notes) { update_status(T("Not enough memory")); return false; }
    const Task *t=find_task(document_,active_);
    bool unchanged=t && t->title==title_->value() && t->project==project_input_->value() && t->due==due_->value() &&
        t->tags==tags_->value() && t->notes==notes && int(t->priority)==priority_->value() && t->completed==(completed_->value()!=0);
    if (unchanged) { free(notes); fields_dirty_=false; apply_->deactivate(); revert_->deactivate(); update_status(T("Ready")); return true; }
    if (!begin_change()) { free(notes); return false; }
    bool ok=update_task(document_,active_,title_->value(),notes,project_input_->value(),due_->value(),Priority(priority_->value()),tags_->value(),completed_->value()!=0);
    free(notes);
    if (!ok) { history_.abandon_checkpoint(); update_status(T("A task needs a nonempty title and valid bounded text")); title_->take_focus(); return false; }
    history_.committed(); fields_dirty_=false; apply_->deactivate(); revert_->deactivate();
    if (refresh) { refill(); refresh_editor(); }
    update_status(T("Task updated")); return true;
}
void TodoWindow::table_selection(int row) {
    if (io_.busy()) return;
    Array<TaskId> selected;
    for (size_t n=0;n<visible_.size();++n) if (table_->row_selected(int(n))) selected.push(document_.tasks[visible_[n]].id);
    selected_.swap(selected);
    active_=row>=0 && size_t(row)<visible_.size() && table_->row_selected(row)?document_.tasks[visible_[size_t(row)]].id:selected_.empty()?0:selected_[0];
    refill(); refresh_editor(); update_status();
}
void TodoWindow::select_relative(int delta) {
    if (!apply_editor() || !visible_.size()) return;
    int at=0; for (size_t n=0;n<visible_.size();++n) if (document_.tasks[visible_[n]].id==active_) at=int(n);
    at+=delta; if (at<0) at=0; if (at>=int(visible_.size())) at=int(visible_.size())-1;
    table_->select_all_rows(0); table_->select_row(at,1); table_->top_row(at); table_selection(at); table_->redraw();
}
void TodoWindow::choose_filter() {
    int choice=filters_->value();
    if (!apply_editor()) { for (int n=0;n<6;++n) if (filter_order[n]==filter_) filters_->value(n+1); return; }
    if (choice>0 && choice<=6) filter_=filter_order[choice-1]; refill(false); refresh_editor();
}
void TodoWindow::choose_project() {
    Text next;
    int at=project_list_->value()-2;
    if (at>=0 && size_t(at)<projects_.size() && !next.set(projects_[size_t(at)].c_str())) { update_status(T("Not enough memory")); return; }
    if (!apply_editor()) { refill(); return; }
    project_=static_cast<Text &&>(next);
    refill(); refresh_editor();
}
void TodoWindow::choose_sort() {
    SortMode mode=SortMode(sort_choice_->value());
    if (!apply_editor()) { sort_choice_->value(int(sort_)); return; }
    if (begin_change()) { bool ok=sort_tasks(document_,mode); if (ok) sort_=mode; finish_change(ok,T("Tasks sorted")); }
}
void TodoWindow::search_tick(void *p) {
    auto *self=static_cast<TodoWindow *>(p);
    if (!self->apply_editor()) { self->search_->value(self->query_.c_str()); return; }
    self->query_.set(self->search_->value()); self->refill(false); self->refresh_editor();
}
void TodoWindow::tick(void *p) { auto *self=static_cast<TodoWindow *>(p); self->poll_io(); Fl::repeat_timeout(.03,tick,p); }
void TodoWindow::lock_document(bool locked) {
    if (locked) { body_->deactivate(); ribbon_->deactivate(); } else { body_->activate(); ribbon_->activate(); refill(); refresh_editor(); }
    update_status();
}
}
