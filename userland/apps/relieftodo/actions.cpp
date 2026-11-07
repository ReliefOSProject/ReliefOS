#include "window.h"
#include <FL/fl_ask.H>
#include <stdio.h>
namespace rt {
bool TodoWindow::choose_path(Text &path,bool saving,const char *filter) {
    const char *value=fl_file_chooser(saving?T("Save task list"):T("Open task list"),filter,path.empty()?nullptr:path.c_str(),0);
    if (!value) return false;
    if (!*value || strlen(value)>4095 || !path.set(value)) { update_status(T("Invalid path or not enough memory")); return false; }
    if (saving) {
        const char *base=strrchr(value,'/'); base=base?base+1:value;
        if (!strchr(base,'.')) {
            const char *ext=!strcmp(filter,"*.csv")?".csv":!strcmp(filter,"*.md")?".md":".rtodo";
            char joined[4104]; snprintf(joined,sizeof(joined),"%s%s",path.c_str(),ext);
            if (strlen(joined)>4095 || !path.set(joined)) { update_status(T("Invalid path or not enough memory")); return false; }
        }
    }
    return true;
}
bool TodoWindow::confirm_replace(Command next) {
    if (!dirty() && !fields_dirty_) return true;
    int choice=fl_choice(T("Save changes to this task list?"),T("Cancel"),T("Discard"),T("Save"));
    if (choice==2) { after_save_=next; save(false); return false; }
    return choice==1;
}
void TodoWindow::reset_document(bool example) {
    Document doc;
    bool ok=example?seed_example(doc):doc.title.set(T("Untitled task list"));
    Document snapshot;
    if (!ok || !snapshot.clone_from(doc)) { update_status(T("Not enough memory")); return; }
    document_=static_cast<Document &&>(doc); saved_=static_cast<Document &&>(snapshot); force_dirty_=example;
    path_.clear(); stamp_=FileStamp{}; history_.clear(); query_.clear(); project_.clear(); search_->value("");
    filter_=Filter::All; sort_=SortMode::Manual; selected_.clear(); active_=0;
    refill(); refresh_editor(); update_status(example?T("Example task list loaded"):T("New task list created"));
}
void TodoWindow::load_initial(const char *path) {
    Text error;
    if (!pending_path_.set(path) || !io_.read(path,error)) { update_status(error.empty()?T("Not enough memory"):error.c_str()); return; }
    lock_document(true); update_status(T("Opening task list..."));
}
void TodoWindow::save(bool as_new) {
    Text error;
    if (!apply_editor(false) || !encode_document(document_,pending_bytes_,error)) {
        if (!error.empty()) update_status(error.c_str()); after_save_=Command::Idle; return;
    }
    pending_kind_=Command::Save;
    pending_path_=path_;
    bool choose=as_new || path_.empty();
    if (choose && !choose_path(pending_path_,true,"*.rtodo")) { after_save_=Command::Idle; pending_bytes_.clear(); return; }
    if (pending_path_==path_ && !path_.empty()) { pending_stamp_=stamp_; start_write(false); }
    else start_write(true);
}
void TodoWindow::export_file(Command format) {
    Text error;
    bool ok=format==Command::ExportCsv?export_csv(document_,pending_bytes_,error):export_markdown(document_,pending_bytes_,error);
    if (!ok) { update_status(error.c_str()); return; }
    pending_kind_=format; pending_path_.clear();
    if (!choose_path(pending_path_,true,format==Command::ExportCsv?"*.csv":"*.md")) { pending_bytes_.clear(); return; }
    if (!path_.empty() && path_==pending_path_) { update_status(T("Choose a different path for the exported file")); pending_bytes_.clear(); return; }
    start_write(true);
}
void TodoWindow::start_write(bool probe) {
    Text error;
    const char *protected_document=pending_kind_==Command::Save?nullptr:path_.c_str();
    bool ok=probe?io_.probe(pending_path_.c_str(),error,protected_document):
        io_.write(pending_path_.c_str(),pending_bytes_,pending_stamp_,error,protected_document);
    if (!ok) { update_status(error.c_str()); after_save_=Command::Idle; pending_bytes_.clear(); lock_document(false); return; }
    lock_document(true); update_status(probe?T("Checking destination..."):T("Saving task list..."));
}
void TodoWindow::poll_io() {
    if (!io_.busy() || !io_.poll()) return;
    IoJob::Kind kind=io_.kind(); IoJob::Reply reply=io_.take();
    if (!reply.ok) {
        lock_document(false); update_status(reply.error.empty()?T("File operation failed"):reply.error.c_str());
        pending_bytes_.clear(); after_save_=Command::Idle; return;
    }
    if (kind==IoJob::Kind::Probe) {
        pending_stamp_=reply.stamp;
        if (reply.stamp.exists && fl_choice(T("The destination already exists. Replace it?"),T("Cancel"),T("Replace"),nullptr)!=1) {
            pending_bytes_.clear(); after_save_=Command::Idle; lock_document(false); update_status(T("Save cancelled")); return;
        }
        start_write(false); return;
    }
    if (kind==IoJob::Kind::Read) {
        Document loaded,snapshot; Text error;
        if (!decode_document(reply.bytes.data(),reply.bytes.size(),loaded,error) || !snapshot.clone_from(loaded)) {
            lock_document(false); update_status(error.empty()?T("Not enough memory"):error.c_str()); return;
        }
        document_=static_cast<Document &&>(loaded); saved_=static_cast<Document &&>(snapshot); force_dirty_=false;
        path_=pending_path_; stamp_=reply.stamp; history_.clear(); query_.clear(); project_.clear(); search_->value("");
        filter_=Filter::All; sort_=SortMode::Manual; selected_.clear(); active_=0;
        lock_document(false); update_status(T("Task list opened"));
        printf("[relieftodo.elf] opened tasks=%zu\n",document_.tasks.size()); fflush(stdout); return;
    }
    Command after=Command::Idle;
    if (pending_kind_==Command::Save) {
        path_=pending_path_; stamp_=reply.stamp;
        if (!saved_.clone_from(document_)) { force_dirty_=true; lock_document(false); update_status(T("File saved, but not enough memory to track saved state")); after_save_=Command::Idle; return; }
        force_dirty_=false; after=after_save_;
        printf("[relieftodo.elf] saved tasks=%zu\n",document_.tasks.size()); fflush(stdout);
    } else { puts("[relieftodo.elf] export complete"); fflush(stdout); }
    after_save_=Command::Idle; pending_bytes_.clear(); lock_document(false);
    update_status(pending_kind_==Command::Save?T("Task list saved"):T("Task list exported"));
    if (after!=Command::Idle) run(after);
}
void TodoWindow::statistics() {
    char today[11]{}; local_date(today); size_t done=0,overdue=0,due_today=0,priority[4]{};
    for (size_t n=0;n<document_.tasks.size();++n) {
        const Task &t=document_.tasks[n]; if (t.completed) ++done;
        if (task_matches(t,Filter::Overdue,"",today)) ++overdue;
        if (task_matches(t,Filter::Today,"",today)) ++due_today;
        if (!t.completed) ++priority[unsigned(t.priority)];
    }
    char text[1024]; snprintf(text,sizeof(text),T("%s\n\nTotal: %zu    Active: %zu    Done: %zu\nDue today: %zu    Overdue: %zu\n\nActive priorities\nUrgent: %zu    High: %zu    Normal: %zu    Low: %zu\n\nProjects: %zu    Visible tasks: %zu"),document_.title.c_str(),document_.tasks.size(),document_.tasks.size()-done,done,due_today,overdue,priority[3],priority[2],priority[1],priority[0],projects_.size(),visible_.size());
    fl_message("%s",text);
}
void TodoWindow::run(Command command) {
    if (command==Command::Idle) return;
    if (io_.busy()) { update_status(T("Please wait for the file operation to finish")); return; }
    if (command==Command::Revert) { refresh_editor(); update_status(T("Unapplied changes reverted")); return; }
    if (!apply_editor(false)) return;
    switch (command) {
    case Command::Close: if (confirm_replace(command)) hide(); return;
    case Command::New: if (confirm_replace(command)) reset_document(false); return;
    case Command::Example: if (confirm_replace(command)) reset_document(true); return;
    case Command::Open: {
        if (!confirm_replace(command)) return;
        Text path=path_; if (choose_path(path,false,"*.rtodo")) load_initial(path.c_str()); return;
    }
    case Command::Save: save(false); return;
    case Command::SaveAs: save(true); return;
    case Command::ExportCsv: case Command::ExportMarkdown: export_file(command); return;
    case Command::Apply: refill(); refresh_editor(); update_status(T("Task updated")); return;
    case Command::Undo: {
        bool ok=history_.undo(document_); refill(); refresh_editor(); update_status(ok?T("Undo complete"):T("Nothing to undo")); return;
    }
    case Command::Redo: {
        bool ok=history_.redo(document_); refill(); refresh_editor(); update_status(ok?T("Redo complete"):T("Nothing to redo")); return;
    }
    case Command::ClearFilters:
        query_.clear(); project_.clear(); search_->value(""); filter_=Filter::All; refill(); refresh_editor(); update_status(T("All filters cleared")); return;
    case Command::SelectAll:
        table_->select_all_rows(1); table_selection(0); table_->redraw(); return;
    case Command::ClearSelection:
        table_->select_all_rows(0); selected_.clear(); active_=0; refresh_editor(); table_->redraw(); return;
    case Command::Statistics: refill(); statistics(); return;
    case Command::Help:
        fl_message("%s",T("ReliefTodo\nNative offline task manager for ReliefOS\n\nCreate tasks with titles, notes, projects, tags, priority and due dates.\nUse the left sidebar to search and filter; Ctrl or Shift selects multiple rows.\nThe right panel edits the active task; Apply changes or Ctrl+Enter commits it.\nRibbon tools apply to every selected task. Undo and redo cover task changes.\n\nCtrl+N: new list    Ctrl+O: open    Ctrl+S: save\nCtrl+F: search    Ctrl+Insert: new task    F1: help\nSpace in the table: toggle completion    Delete in the table: delete\n\nSave editable lists as .rtodo; export all tasks as CSV or Markdown.\nDates follow the system local date. No background alarms or cloud sync.")); return;
    case Command::Rename: {
        const char *name=fl_input(T("Task list title:"),document_.title.c_str()); if (!name) return;
        if (strlen(name)>256 || !valid_utf8(name,strlen(name))) { update_status(T("Invalid task list title")); return; }
        Text next; if (!next.set(name) || !begin_change()) return;
        document_.title=next; ++document_.revision; finish_change(true,T("Task list renamed")); return;
    }
    case Command::Add: {
        if (!begin_change()) return;
        TaskId id=0; char today[11]{}; local_date(today);
        bool ok=add_task(document_,T("New task"),"",project_.c_str(),filter_==Filter::Today?today:"",Priority::Normal,"",id);
        if (ok) {
            filter_=Filter::All; query_.clear(); search_->value(""); selected_.clear(); selected_.push(id); active_=id; sort_=SortMode::Manual;
            sort_tasks(document_,sort_);
        }
        finish_change(ok,T("New task created"));
        if (ok) { title_->take_focus(); title_->position(0,int(strlen(title_->value()))); for (size_t n=0;n<visible_.size();++n) if (document_.tasks[visible_[n]].id==id) table_->top_row(int(n)); }
        return;
    }
    default: break;
    }
    if (!selected_.size()) { update_status(T("Select one or more tasks first")); return; }
    if (command==Command::Delete && fl_choice(T("Delete the selected tasks? You can undo this action."),T("Cancel"),T("Delete"),nullptr)!=1) return;
    Text group,tags;
    if (command==Command::Project || command==Command::Tags) {
        const Task *t=find_task(document_,active_);
        const char *s=fl_input(command==Command::Project?T("Project (empty removes it):"):T("Tags, separated by commas (empty removes them):"),t?(command==Command::Project?t->project.c_str():t->tags.c_str()):"");
        if (!s) return;
        if (!(command==Command::Project?group:tags).set(s)) { update_status(T("Not enough memory")); return; }
    }
    if ((command==Command::MoveUp || command==Command::MoveDown) && selected_.size()!=1) { update_status(T("Select a single task to change its manual order")); return; }
    if (!begin_change()) return;
    bool ok=true; TaskId new_id=active_; size_t count=selected_.size();
    for (size_t n=0;ok && n<count;++n) {
        TaskId id=selected_[n]; const Task *old=find_task(document_,id); if (!old) { ok=false; break; }
        Task t=*old;
        switch (command) {
        case Command::Delete: ok=delete_task(document_,id); break;
        case Command::Duplicate: ok=copy_task(document_,id,new_id); break;
        case Command::Done: case Command::Restore: case Command::Toggle:
            ok=set_task_done(document_,id,command==Command::Toggle?!t.completed:command==Command::Done); break;
        case Command::MoveUp: case Command::MoveDown:
            ok=move_task(document_,id,command==Command::MoveUp?-1:1); if (ok) sort_=SortMode::Manual; break;
        default: {
            if (command==Command::Low) t.priority=Priority::Low;
            if (command==Command::Normal) t.priority=Priority::Normal;
            if (command==Command::High) t.priority=Priority::High;
            if (command==Command::Urgent) t.priority=Priority::Urgent;
            if (command==Command::Project) t.project=group;
            if (command==Command::Tags) t.tags=tags;
            if (command==Command::Today || command==Command::Tomorrow || command==Command::NextWeek) {
                char date[11]{}; ok=local_date(date,command==Command::Today?0:command==Command::Tomorrow?1:7) && t.due.set(date);
            }
            if (command==Command::ClearDate) t.due.clear();
            if (ok) ok=update_task(document_,id,t.title.c_str(),t.notes.c_str(),t.project.c_str(),t.due.c_str(),t.priority,t.tags.c_str(),t.completed);
            break;
        }
        }
    }
    if (ok && command==Command::Duplicate) { selected_.clear(); selected_.push(new_id); active_=new_id; filter_=Filter::All; }
    if (ok && sort_!=SortMode::Manual) ok=sort_tasks(document_,sort_);
    finish_change(ok,T("Selected tasks updated"));
}
int TodoWindow::handle(int event) {
    if ((event==FL_KEYDOWN || event==FL_SHORTCUT) && !Fl::compose_state) {
        int key=Fl::event_key(),state=Fl::event_state();
        if ((state&FL_CTRL) && key==FL_Enter) { run(Command::Apply); return 1; }
        if ((state&FL_CTRL) && key==FL_Insert) { run(Command::Add); return 1; }
        if ((state&FL_CTRL) && key=='f') { if (!io_.busy()) search_->take_focus(); return 1; }
        if ((state&FL_CTRL) && key=='s') { run(state&FL_SHIFT?Command::SaveAs:Command::Save); return 1; }
        if ((state&FL_CTRL) && key=='o') { run(Command::Open); return 1; }
        if ((state&FL_CTRL) && key=='n') { run(Command::New); return 1; }
        if (event==FL_SHORTCUT && (state&FL_CTRL) && key=='z') { run(Command::Undo); return 1; }
        if (event==FL_SHORTCUT && (state&FL_CTRL) && key=='y') { run(Command::Redo); return 1; }
    }
    return Fl_Double_Window::handle(event);
}
}
