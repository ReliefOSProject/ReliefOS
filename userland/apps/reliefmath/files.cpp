#include "window.h"
#include "jobs.h"
#include "export.h"
#include "recovery_flow.h"
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Return_Button.H>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
namespace rm {
struct MathWindow::FileState {
    FileStamp stamp;
    RecoveryStore recovery;
    RecoveryFlow recovery_flow;
    bool recovery_ready=false,offered=false,approved=false,cleared=false;
    bool startup_recovery_after_open=false;
    bool export_finished=false;
    bool save_cleanup_pending=false,save_cleanup_recovered=false;
    uint64_t save_cleanup_id=0;
    int continuation=-1;
    uint64_t recovered_id=0;
    Text pending_open,export_directory;
    Fl_Double_Window *progress=nullptr;
    Fl_Hold_Browser *results=nullptr;
    Fl_Box *progress_label=nullptr;
    void abandon_continuation(){
        if(continuation==int(Command::Open))pending_open.clear();
        continuation=-1;approved=false;recovery_flow.abandon();
    }
};
namespace {
struct Decision {int *result;int value;};
int conflict_dialog(){
    Fl_Double_Window dialog(600,166,T("File changed externally"));
    Fl_Box label(16,14,568,70,T("The file differs from the version you opened.\nReload it, save to another path, or explicitly replace it."));label.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_WRAP);
    int result=-1;Decision decisions[4];const char *labels[]={T("Reload"),T("Save as..."),T("Overwrite"),T("Cancel")};
    for(int n=0;n<4;n++){auto *b=new Fl_Button(16+n*146,114,136,30,labels[n]);decisions[n]={&result,n};b->callback([](Fl_Widget*,void *p){auto &d=*static_cast<Decision*>(p);*d.result=d.value;},&decisions[n]);}
    dialog.end();reliefos_fltk::style(&dialog);dialog.set_modal();dialog.show();while(dialog.shown()&&result<0)Fl::wait(.03);dialog.hide();return result;
}
const char *status_label(ExportStatus s){switch(s){case ExportStatus::Pending:return T("Queued");case ExportStatus::Rendering:return T("Rendering");case ExportStatus::Writing:return T("Saving");case ExportStatus::Done:return T("Done");case ExportStatus::Failed:return T("Failed");case ExportStatus::Cancelled:return T("Cancelled");}return "";}
}
void MathWindow::init_files(){files_=new FileState;Error e;if(!io_->recovery(IoKind::RecoveryInit,files_->recovery,nullptr,nullptr,0,e))update_status(e.message.c_str());}
void MathWindow::destroy_files(){if(files_){delete files_->progress;delete files_;files_=nullptr;}}
void MathWindow::start_recovery_load(uint64_t id){
    if(!files_||!id)return;Error e;
    if(!io_->recovery(IoKind::RecoveryLoad,files_->recovery,nullptr,nullptr,id,e)){
        files_->recovery_flow.load_start_failed();update_status(e.message.c_str());
    }else {document_locked_=true;editor->deactivate();}
}
void MathWindow::start_saved_recovery_cleanup(){
    if(!files_||!files_->save_cleanup_pending||io_->pending())return;
    RecoveryFlow::CleanupTarget target=RecoveryFlow::next_cleanup_target(
        files_->recovered_id,files_->recovery.current_id()!=0);
    if(target.kind==RecoveryFlow::CleanupTarget::Kind::None){
        files_->save_cleanup_pending=false;files_->save_cleanup_recovered=false;files_->save_cleanup_id=0;
        document_locked_=false;editor->activate();update_status(T("Document saved"));return;
    }
    files_->save_cleanup_recovered=target.kind==RecoveryFlow::CleanupTarget::Kind::Recovered;
    files_->save_cleanup_id=target.id;
    if(files_->save_cleanup_recovered)files_->recovered_id=0;
    document_locked_=true;editor->deactivate();Error e;
    if(io_->recovery(IoKind::RecoveryDiscard,files_->recovery,nullptr,nullptr,target.id,e)){
        update_status(T("Clearing recovery snapshot..."));return;
    }
    if(files_->save_cleanup_recovered)files_->recovered_id=target.id;
    files_->save_cleanup_pending=false;files_->save_cleanup_recovered=false;files_->save_cleanup_id=0;
    document_locked_=false;editor->activate();update_status(e.message.c_str());
}
bool MathWindow::finish_dirty(){
    flush_edit();if(!history.is_dirty(document))return true;
    int answer=fl_choice("%s",T("Cancel"),T("Discard"),T("Save"),T("Save changes before continuing?"));
    if(answer==1)return true;
    if(answer==2){file_action(Command::Save);return false;}
    files_->abandon_continuation();return false;
}
void MathWindow::load_initial(const char *path){
    if(!path||!*path)return;
    if(!files_->pending_open.set(path)){update_status(T("Cannot store file path"));return;}
    files_->startup_recovery_after_open=true;
    if(io_->pending()){files_->continuation=int(Command::Open);return;}
    file_action(Command::Open);
}
void MathWindow::file_action(Command command){
    UiGuard guard(interaction_depth_);
    if(!files_)return;
    if(io_->pending()){update_status(T("A file task is running; wait or cancel it"));return;}
    if(command==Command::Save||command==Command::SaveAs){
        flush_edit();Text target=path_;bool new_path=command==Command::SaveAs||target.empty();
        if(new_path){
            const char *chosen=fl_file_chooser(T("Save formula document"),"*.rmath",target.empty()?"Untitled.rmath":target.c_str(),0);
            if(!chosen){files_->abandon_continuation();return;}char buffer[4096];const char *ext=strrchr(chosen,'.');
            int count=snprintf(buffer,sizeof buffer,"%s%s",chosen,ext&&!strcasecmp(ext,".rmath")?"":".rmath");
            if(count<0||count>=int(sizeof buffer)||!target.set(buffer)){update_status(T("File path is too long"));files_->abandon_continuation();return;}
        }
        Error e;FileStamp expected=new_path?FileStamp{}:files_->stamp;
        if(io_->save(target.c_str(),document,expected,false,session_,e))update_status(T("Saving document in background..."));
        else {update_status(e.message.c_str());files_->abandon_continuation();}return;
    }
    if(command==Command::Open&&files_->pending_open.empty()){
        const char *chosen=fl_file_chooser(T("Open formula document"),"*.rmath",path_.empty()?nullptr:path_.c_str(),1);
        if(!chosen){files_->abandon_continuation();return;}if(!files_->pending_open.set(chosen)){update_status(T("Cannot store file path"));files_->abandon_continuation();return;}
    }
    files_->continuation=int(command);
    if(!files_->approved&&!finish_dirty())return;
    files_->approved=true;
    if(exports_->busy()){
        int answer=fl_choice("%s",T("Keep working"),T("Cancel export"),nullptr,T("An export is running. Cancel it before switching documents?"));
        if(answer!=1){files_->abandon_continuation();return;}exports_->cancel();if(exports_->busy())return;
    }
    if(files_->recovery_ready&&!files_->cleared){
        Error e;
        if(files_->recovery.current_id()||files_->recovered_id){
            uint64_t id=files_->recovered_id;files_->recovered_id=0;
            if(io_->recovery(IoKind::RecoveryDiscard,files_->recovery,nullptr,nullptr,id,e)){update_status(T("Clearing recovery snapshot..."));return;}
            update_status(e.message.c_str());files_->abandon_continuation();return;
        }
        files_->cleared=true;
    }
    files_->continuation=-1;files_->approved=false;files_->cleared=false;
    if(command==Command::Close){renderer_->cancel();if(files_->progress)files_->progress->hide();hide();return;}
    if(command==Command::Open){
        Error e;Text next=files_->pending_open;files_->pending_open.clear();
        if(io_->load(next.c_str(),session_,e)){document_locked_=true;editor->deactivate();update_status(T("Opening document in background..."));}
        else {
            update_status(e.message.c_str());
            if(files_->startup_recovery_after_open){files_->startup_recovery_after_open=false;offer_recovery();}
        }return;
    }
    Document next;next.title.set(command==Command::Example?T("Example formulas"):T("Untitled"));FormulaId id=0;Error e;
    if(command==Command::Example){
        const char *names[]={T("Quadratic formula"),T("Gaussian integral"),T("Matrix"),T("Cases"),T("Aligned equations"),T("Chinese formula"),T("Euler identity")};
        const char *sources[]={"x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}","\\int_{-\\infty}^{\\infty} e^{-x^2}\\,dx=\\sqrt{\\pi}","A=\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}","f(x)=\\begin{cases}x^2 & x\\geq 0\\\\-x & x<0\\end{cases}","\\begin{aligned}a^2+b^2&=c^2\\\\E&=mc^2\\end{aligned}","\\text{中文公式}:\\quad S=\\sum_{k=1}^{n}k=\\frac{n(n+1)}{2}","e^{i\\pi}+1=0"};
        for(size_t n=0;n<sizeof(names)/sizeof(names[0]);n++)if(!add_formula(next,names[n],sources[n],id,e)){update_status(e.message.c_str());return;}
    }else if(!add_formula(next,T("Formula 1"),"E=mc^2",id,e)){update_status(e.message.c_str());return;}
    document=static_cast<Document&&>(next);history=History{};if(command!=Command::Example)history.mark_saved(document,e);
    session_++;active_=document.formulas[0].id;renderer_->cancel();preview->clear();path_.clear();files_->stamp=FileStamp{};filter_->value("");refill_list();refresh_source();update_status(command==Command::Example?T("Example document · save it as a new file"):T("Ready"));
}
void MathWindow::finish_file_job(){
    IoResult result;if(!io_->take(result)){
        if(!io_->pending()&&files_->continuation>=0&&!exports_->busy()){Command next=Command(files_->continuation);file_action(next);}return;
    }
    if(result.kind==IoKind::RecoveryInit){
        files_->recovery_ready=result.success;
        if(!result.success)update_status(result.error.message.c_str());
        if(files_->continuation>=0){Command next=Command(files_->continuation);files_->continuation=-1;file_action(next);}else offer_recovery();return;
    }
    if(result.kind==IoKind::RecoveryDiscard){
        if(files_->save_cleanup_pending){
            if(!result.success){
                if(files_->save_cleanup_recovered){
                    for(size_t n=0;n<files_->recovery.items().size();n++)
                        if(files_->recovery.items()[n].id==files_->save_cleanup_id){files_->recovered_id=files_->save_cleanup_id;break;}
                }
                files_->save_cleanup_pending=false;files_->save_cleanup_recovered=false;files_->save_cleanup_id=0;
                document_locked_=false;editor->activate();update_status(result.error.message.c_str());return;
            }
            files_->save_cleanup_recovered=false;files_->save_cleanup_id=0;
            start_saved_recovery_cleanup();return;
        }
        if(!result.success){update_status(result.error.message.c_str());files_->abandon_continuation();return;}
        if(files_->continuation>=0){Command next=Command(files_->continuation);file_action(next);}return;
    }
    if(result.kind==IoKind::RecoverySave){update_status(result.success?T("Recovery snapshot saved"):result.error.message.c_str());return;}
    if(result.kind==IoKind::RecoveryLoad){
        document_locked_=false;editor->activate();
        uint64_t recovered_id=files_->recovery_flow.finish_load();
        if(result.success){if(recovered_id)files_->recovered_id=recovered_id;files_->cleared=false;document=static_cast<Document&&>(result.document);history=History{};path_.clear();files_->stamp=FileStamp{};session_++;active_=document.formulas.empty()?0:document.formulas[0].id;preview->clear();renderer_->cancel();refill_list();refresh_source();update_status(T("Recovered into a new document · choose Save as"));}
        else update_status(result.error.message.c_str());return;
    }
    if(result.kind==IoKind::Load){
        document_locked_=false;editor->activate();
        if(result.success&&result.session==session_){document=static_cast<Document&&>(result.document);history=History{};Error e;history.mark_saved(document,e);path_=result.path;files_->stamp=result.stamp;session_++;active_=document.formulas.empty()?0:document.formulas[0].id;preview->clear();renderer_->cancel();filter_->value("");refill_list();refresh_source();update_status(T("Document opened"));}
        else update_status(result.error.message.c_str());
        if(files_->startup_recovery_after_open){files_->startup_recovery_after_open=false;offer_recovery();}return;
    }
    if(result.kind==IoKind::Save){
        if(result.success&&result.session==session_){
            path_=result.path;files_->stamp=result.stamp;flush_edit();Error e;bool marked=history.mark_saved(result.document,e);bool dirty=history.is_dirty(document);
            if(!marked)update_status(e.message.c_str());else update_status(dirty?T("Snapshot saved · newer edits remain unsaved"):T("Document saved"));
            bool recovery_waiting=files_->recovery_flow.waiting_for_save();
            uint64_t recovery_id=files_->recovery_flow.save_result(RecoveryFlow::SaveResult::Succeeded,dirty||!marked);
            if(recovery_waiting&&!recovery_id&&(dirty||!marked)){
                UiGuard guard(interaction_depth_);
                if(!finish_dirty())return;
                recovery_id=files_->recovery_flow.accept_after_dirty_check();
            }
            bool has_snapshots=files_->recovery_ready&&(files_->recovered_id||files_->recovery.current_id());
            RecoveryFlow::SaveFollowup followup=RecoveryFlow::after_save(
                recovery_id!=0,files_->continuation>=0,dirty||!marked,has_snapshots);
            if(followup==RecoveryFlow::SaveFollowup::LoadRecovery){start_recovery_load(recovery_id);return;}
            if(followup==RecoveryFlow::SaveFollowup::ContinueCommand){Command next=Command(files_->continuation);file_action(next);return;}
            if(followup==RecoveryFlow::SaveFollowup::UpdateDirtySnapshot){recovery_tick();return;}
            if(followup==RecoveryFlow::SaveFollowup::ClearSnapshots){files_->save_cleanup_pending=true;start_saved_recovery_cleanup();return;}
            return;
        }
        if(result.error.code==Code::Conflict){
            UiGuard guard(interaction_depth_);
            int answer=conflict_dialog();Error e;
            if(answer==0){files_->recovery_flow.save_result(RecoveryFlow::SaveResult::Cancelled);files_->abandon_continuation();files_->pending_open=result.path;files_->approved=true;file_action(Command::Open);}
            else if(answer==1){files_->recovery_flow.save_result(RecoveryFlow::SaveResult::SaveAsRetry);file_action(Command::SaveAs);}
            else if(answer==2){files_->recovery_flow.save_result(RecoveryFlow::SaveResult::OverwriteRetry);flush_edit();if(!io_->save(result.path.c_str(),document,FileStamp{},true,session_,e)){update_status(e.message.c_str());files_->abandon_continuation();}}
            else {files_->recovery_flow.save_result(RecoveryFlow::SaveResult::Cancelled);files_->abandon_continuation();update_status(T("Save cancelled · original file kept"));}return;
        }
        files_->recovery_flow.save_result(RecoveryFlow::SaveResult::Failed);files_->abandon_continuation();update_status(result.error.message.c_str());
    }
}
void MathWindow::recovery_tick(){
    if(!files_||!files_->recovery_ready||io_->pending()||document_locked_||!history.is_dirty(document))return;
    flush_edit();Error e;if(!io_->recovery(IoKind::RecoverySave,files_->recovery,&document,path_.c_str(),0,e))update_status(e.message.c_str());
}
void MathWindow::offer_recovery(){
    UiGuard guard(interaction_depth_);
    if(files_->offered||!files_->recovery_ready)return;files_->offered=true;
    if(files_->recovery.items().empty())return;
    Fl_Double_Window dialog(650,320,T("Recover formula document"));
    Fl_Box heading(16,8,618,32,T("A previous session left recoverable formula documents."));heading.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
    Fl_Hold_Browser list(16,48,618,194);list.textsize(14);
    for(size_t n=0;n<files_->recovery.items().size();n++){const auto &r=files_->recovery.items()[n];char row[1024];snprintf(row,sizeof row,"%s    %s",r.title.empty()?T("Untitled"):r.title.c_str(),r.original_path_hint.c_str());list.add(row);}list.value(1);
    int action=-1;Decision choices[3];const char *labels[]={T("Recover as new"),T("Keep for later"),T("Discard snapshot")};
    for(int n=0;n<3;n++){auto *b=new Fl_Button(16+n*210,266,198,32,labels[n]);choices[n]={&action,n};b->callback([](Fl_Widget*,void *p){auto &d=*static_cast<Decision*>(p);*d.result=d.value;},&choices[n]);}
    dialog.end();reliefos_fltk::style(&dialog);dialog.set_modal();dialog.show();while(dialog.shown()&&action<0)Fl::wait(.03);dialog.hide();
    int index=list.value();if(index<=0||size_t(index)>files_->recovery.items().size())return;uint64_t id=files_->recovery.items()[size_t(index-1)].id;Error e;
    if(action==0){
        if(!files_->recovery_flow.select(id))return;
        if(!finish_dirty()){
            if(io_->pending()){if(!files_->recovery_flow.wait_for_save())files_->recovery_flow.abandon();}
            else files_->recovery_flow.abandon();
            return;
        }
        start_recovery_load(files_->recovery_flow.begin_load());
    }
    else if(action==2&&!io_->recovery(IoKind::RecoveryDiscard,files_->recovery,nullptr,nullptr,id,e))update_status(e.message.c_str());
}
void MathWindow::export_action(Command command){
    if(exports_->busy()){if(files_->progress)files_->progress->show();update_status(T("An export is already running"));return;}
    flush_edit();Fl_Double_Window dialog(690,434,T("Export formulas"));
    Fl_Choice scope(142,18,520,28,T("Formulas:"));scope.add(T("Current formula"));scope.add(T("Selected formulas"));scope.add(T("All formulas"));scope.value(command==Command::ExportAll?2:command==Command::ExportSelected?1:0);
    Fl_Choice format(142,58,250,28,T("Format:"));format.add("SVG");format.add(T("Transparent PNG"));format.add(T("LaTeX source"));format.value(command==Command::ExportPng?1:command==Command::ExportLatex?2:0);
    Fl_Input directory(142,98,420,28,T("Directory:"));directory.value(files_->export_directory.empty()?(getenv("HOME")?getenv("HOME"):"/"):files_->export_directory.c_str());
    Fl_Button browse(570,98,92,28,T("Browse..."));browse.callback([](Fl_Widget*,void *p){auto *in=static_cast<Fl_Input*>(p);const char *chosen=fl_dir_chooser(T("Export directory"),in->value(),1);if(chosen)in->value(chosen);},&directory);
    Fl_Input pattern(142,138,520,28,T("Filename pattern:"));pattern.value("{index}-{name}");
    Fl_Box naming(142,170,520,30,T("Use {index} and {name}; the extension is added automatically."));naming.labelsize(12);naming.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_WRAP);
    Fl_Value_Input scale(142,212,120,28,T("Scale:"));scale.range(.05,16);scale.step(.25);scale.value(1);
    Fl_Value_Input dpi(432,212,120,28,T("DPI:"));dpi.range(36,1200);dpi.step(1);dpi.value(96);
    Fl_Check_Button overwrite(142,254,520,28,T("Replace existing output files"));overwrite.value(0);
    Fl_Box note(16,294,646,62,T("Exports use a source snapshot. Editing can continue.\nEach result is reported separately; cancelling keeps completed files."));note.align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_WRAP);note.labelsize(13);
    int answer=-1;Decision d[2]={{&answer,0},{&answer,1}};Fl_Button cancel(426,378,112,32,T("Cancel"));Fl_Return_Button start(550,378,112,32,T("Export"));
    auto decide=[](Fl_Widget*,void *p){auto &v=*static_cast<Decision*>(p);*v.result=v.value;};cancel.callback(decide,&d[0]);start.callback(decide,&d[1]);
    dialog.end();reliefos_fltk::style(&dialog);dialog.set_modal();dialog.show();while(dialog.shown()&&answer<0)Fl::wait(.03);dialog.hide();if(answer!=1)return;
    ExportOptions options;options.directory.set(directory.value());options.pattern.set(pattern.value());options.scale=scale.value();options.dpi=uint32_t(dpi.value());options.overwrite=overwrite.value()!=0;options.format=format.value()==0?ExportFormat::Svg:format.value()==1?ExportFormat::Png:ExportFormat::Latex;
    Array<FormulaId> ids;if(scope.value()==0)ids.push(active_);else if(scope.value()==1)for(int n=1;n<=list_->size();n++)if(list_->selected(n))ids.push(visible_ids_[size_t(n-1)]);
    if(scope.value()!=2&&ids.empty()){update_status(T("Select at least one formula"));return;}
    Error e;if(!exports_->start("/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf",document,ids.data(),ids.size(),session_,options,e)){update_status(e.message.c_str());return;}
    files_->export_directory=options.directory;files_->export_finished=false;
    if(!files_->progress){
        files_->progress=new Fl_Double_Window(780,428,T("Export results"));files_->progress_label=new Fl_Box(16,10,748,30);files_->progress_label->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
        files_->results=new Fl_Hold_Browser(16,52,748,286);files_->results->textsize(13);
        auto *open=new Fl_Button(16,368,156,32,T("Open directory"));open->callback([](Fl_Widget*,void *p){auto *s=static_cast<FileState*>(p);pid_t child=fork();if(child==0){execl("/usr/bin/fileman","fileman",s->export_directory.c_str(),static_cast<char*>(nullptr));_exit(127);}if(child>0)Fl::add_timeout(.2,[](void *v){pid_t id=pid_t(intptr_t(v));waitpid(id,nullptr,WNOHANG);},reinterpret_cast<void*>(intptr_t(child)));},files_);
        auto *stop=new Fl_Button(486,368,136,32,T("Cancel export"));stop->callback([](Fl_Widget*,void *p){static_cast<MathWindow*>(p)->exports_->cancel();},this);
        auto *close=new Fl_Button(634,368,130,32,T("Close"));close->callback([](Fl_Widget *w,void*){w->window()->hide();});files_->progress->end();reliefos_fltk::style(files_->progress);
    }
    files_->progress->show();update_status(T("Export started in background"));
}
void MathWindow::export_progress(){
    if(!files_||!files_->progress||files_->export_finished)return;
    files_->results->clear();size_t good=0,bad=0,cancelled=0;
    for(size_t n=0;n<exports_->items().size();n++){
        const auto &item=exports_->items()[n];char row[6144];snprintf(row,sizeof row,"%s | %s%s%s",status_label(item.status),item.path.c_str(),item.error.message.empty()?"":" | ",item.error.message.empty()?"":T(item.error.message.c_str()));files_->results->add(row);
        good+=item.status==ExportStatus::Done;bad+=item.status==ExportStatus::Failed;cancelled+=item.status==ExportStatus::Cancelled;
    }
    char summary[256];snprintf(summary,sizeof summary,T("%zu completed · %zu failed · %zu cancelled · %zu total"),good,bad,cancelled,exports_->items().size());files_->progress_label->copy_label(summary);
    if(!exports_->busy()){files_->export_finished=true;update_status(summary);}
}
}
