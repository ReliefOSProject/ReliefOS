#include "window.h"
#include "view.h"
#include "../fltk_support.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
namespace rw {
namespace {
void remove_copy(Text &path,int &descriptor) {
    if(!path.empty()&&descriptor>=0) {
        unlink(path.c_str()); char lock[4096];
        int n=snprintf(lock,sizeof(lock),"%s.lock",path.c_str()); if(n>0&&n<int(sizeof(lock))) unlink(lock);
    }
    unlock_recovery_path(descriptor); path.clear();
}
bool release_unlocked_reservation(Text &path) {
    if(path.empty()) return true;
    if(unlink(path.c_str())<0&&errno!=ENOENT) return false;
    path.clear(); return true;
}
bool same_file_identity(const FileStamp &a,const FileStamp &b) {
    return a.exists&&b.exists&&a.device==b.device&&a.inode==b.inode;
}
FileFormat path_format(const char *path) {
    const char *extension=strrchr(path,'.');
    if(extension&&!strcasecmp(extension,".txt")) return FileFormat::Text;
    if(extension&&(!strcasecmp(extension,".html")||!strcasecmp(extension,".htm"))) return FileFormat::Html;
    return FileFormat::Native;
}
}
bool WordWindow::confirm_discard() {
    if(editor.doc.revision==saved_revision_) return true;
    int choice=fl_choice("%s",T("Cancel"),T("Discard"),T("Save"),T("Save changes before continuing?"));
    return choice==1||(choice==2&&save(false));
}
bool WordWindow::choose_path(Text &output,bool saving,FileFormat format) {
    const char *filter=format==FileFormat::Text?"*.txt":format==FileFormat::Html?"*.{html,htm}":saving?"*.rword":"*.{rword,txt,html,htm}";
    const char *initial=path_.empty()?nullptr:path_.c_str();
    const char *selected=fl_file_chooser(saving?T("Save document"):T("Open document"),filter,initial,saving?0:1);
    if(!selected||!*selected) return false;
    Text path; if(!path.set(selected)) { message(T("Not enough memory")); return false; }
    if(saving) {
        const char *suffix=format==FileFormat::Text?".txt":format==FileFormat::Html?".html":".rword";
        const char *extension=strrchr(path.c_str(),'.');
        bool matches=extension&&!strcasecmp(extension,suffix);
        if(format==FileFormat::Html&&extension&&!strcasecmp(extension,".htm")) matches=true;
        if(!matches) {
            char extended[4096]; int n=snprintf(extended,sizeof(extended),"%s%s",path.c_str(),suffix);
            if(n<0||n>=int(sizeof(extended))||!path.set(extended)) { message(T("File path is too long")); return false; }
        }
    }
    output=path; return true;
}
void WordWindow::discard_recovery() {
    if(recovery_descriptor_>=0) remove_copy(recovery_path_,recovery_descriptor_);
    else release_unlocked_reservation(recovery_path_);
    remove_copy(recovered_from_,recovered_descriptor_); recovery_revision_=0;
}
bool WordWindow::save(bool as_new) {
    Text target=path_,error;
    if((as_new||target.empty())&&!choose_path(target,true,FileFormat::Native)) return false;
    FileStamp expected;
    if(target==path_&&!as_new) expected=stamp_;
    else {
        if(!file_stamp(target.c_str(),expected,error)) { fl_alert("%s",T(error.c_str())); return false; }
        if(expected.exists&&fl_choice(T("Replace the existing file?\n%s"),T("Cancel"),T("Replace"),nullptr,target.c_str())!=1) return false;
    }
    busy_=true; cursor(FL_CURSOR_WAIT); message(T("Saving document...")); Fl::flush();
    bool ok=save_document(target.c_str(),editor.doc,FileFormat::Native,error,&expected);
    busy_=false; cursor(FL_CURSOR_DEFAULT);
    if(!ok) {
        FileStamp current; Text check;
        if(file_stamp(target.c_str(),current,check)&&!same_stamp(current,expected)) {
            int choice=fl_choice("%s",T("Cancel"),T("Save a copy..."),T("Replace"),T("The file changed outside ReliefWord. Save a copy or explicitly replace it."));
            if(choice==1) return save(true);
            if(choice==2) { busy_=true; cursor(FL_CURSOR_WAIT); ok=save_document(target.c_str(),editor.doc,FileFormat::Native,error,&current); busy_=false; cursor(FL_CURSOR_DEFAULT); }
        }
    }
    if(!ok) { fl_alert("%s",T(error.c_str())); message(T("Document was not saved")); return false; }
    path_=target;
    if(!file_stamp(target.c_str(),stamp_,error)) { fl_alert("%s",T(error.c_str())); return false; }
    saved_revision_=editor.doc.revision; discard_recovery(); selected(); message(T("Document saved")); view->take_focus(); return true;
}
void WordWindow::open_file(const char *path,FileFormat format) {
    Text error,warning,candidate_path,candidate_import_path; FileStamp before,after;
    if(format==FileFormat::Native) {
        if(!candidate_path.set(path)) { error.set("Not enough memory to retain the opened file path"); fl_alert("%s",T(error.c_str())); return; }
    } else if(!candidate_import_path.set(path)) {
        error.set("Not enough memory to retain the import source path"); fl_alert("%s",T(error.c_str())); return;
    }
    if(!file_stamp(path,before,error)) { fl_alert("%s",T(error.c_str())); return; }
    Document replacement; busy_=true; cursor(FL_CURSOR_WAIT); message(T("Opening document...")); Fl::flush();
    bool ok=load_document(path,format,replacement,error,&warning);
    if(ok) ok=file_stamp(path,after,error);
    if(ok&&!same_stamp(before,after)) { error.set("The file changed while it was being opened"); ok=false; }
    busy_=false; cursor(FL_CURSOR_DEFAULT);
    if(!ok) { fl_alert("%s",T(error.c_str())); message(T("Could not open the document")); return; }
    if(!warning.empty()&&fl_choice("%s",T("Cancel"),T("Import"),nullptr,T(warning.c_str()))!=1) return;
    if(!editor.replace_document(static_cast<Document &&>(replacement))) { fl_alert("%s",T(editor.error.c_str())); return; }
    discard_recovery(); statistics_revision_=0;
    if(format==FileFormat::Native) {
        path_=static_cast<Text &&>(candidate_path); stamp_=after; saved_revision_=editor.doc.revision;
        import_path_.clear(); import_stamp_={};
    } else {
        path_.clear(); stamp_={}; saved_revision_=0;
        import_path_=static_cast<Text &&>(candidate_import_path); import_stamp_=after;
    }
    view->reflow(true); changed();
    if(format!=FileFormat::Native&&view->layout.error.empty()) message(T("Imported — save as .rword to keep formatting"));
}
void WordWindow::load_initial(const char *path) {
    if(path&&*path&&confirm_discard()) open_file(path,path_format(path));
}
void WordWindow::export_file(FileFormat format) {
    if(format==FileFormat::Text&&fl_choice("%s",T("Cancel"),T("Export"),nullptr,T("TXT keeps text only. Character formatting, paragraph layout and page settings are not saved. Your original document remains open."))!=1) return;
    Text target,error; if(!choose_path(target,true,format)) return;
    FileStamp expected; if(!file_stamp(target.c_str(),expected,error)) { fl_alert("%s",T(error.c_str())); return; }
    if(!import_path_.empty()&&(target==import_path_||same_file_identity(expected,import_stamp_))) {
        fl_alert("%s",T("Exporting over the imported source could discard content ReliefWord did not preserve"));
        message(T("Choose a different export file")); return;
    }
    if(expected.exists&&fl_choice(T("Replace the existing file?\n%s"),T("Cancel"),T("Replace"),nullptr,target.c_str())!=1) return;
    busy_=true; cursor(FL_CURSOR_WAIT); message(T("Exporting document...")); Fl::flush();
    bool ok=save_document(target.c_str(),editor.doc,format,error,&expected); busy_=false; cursor(FL_CURSOR_DEFAULT);
    if(!ok) fl_alert("%s",T(error.c_str()));
    message(ok?T("Document exported; original remains open"):T("Could not export the document")); view->take_focus();
}
void WordWindow::recovery_tick(void *context) {
    WordWindow *window=static_cast<WordWindow *>(context);
    if(!window->busy_&&window->shown()&&window->editor.doc.revision!=window->saved_revision_&&window->editor.doc.revision!=window->recovery_revision_) {
        Text error; bool ok=true;
        if(window->recovery_descriptor_<0) {
            ok=release_unlocked_reservation(window->recovery_path_);
        }
        if(ok&&window->recovery_path_.empty()) {
            ok=reserve_recovery_path(window->recovery_path_,error);
            if(ok&&!lock_recovery_path(window->recovery_path_.c_str(),window->recovery_descriptor_,error)) {
                release_unlocked_reservation(window->recovery_path_); ok=false;
            }
        }
        if(ok&&window->recovery_descriptor_>=0&&!window->recovery_path_.empty()) {
            window->busy_=true; ok=save_document(window->recovery_path_.c_str(),window->editor.doc,FileFormat::Native,error); window->busy_=false;
        } else ok=false;
        if(ok) { window->recovery_revision_=window->editor.doc.revision; window->message(T("Recovery copy saved")); }
        else window->message(T("Could not save the recovery copy"));
    }
    Fl::repeat_timeout(60,recovery_tick,window);
}
void WordWindow::offer_recovery() {
    Text directory,error; if(!recovery_directory(directory,error)) return;
    DIR *dir=opendir(directory.c_str()); if(!dir) return;
    Text candidate; int descriptor=-1;
    for(struct dirent *entry;(entry=readdir(dir));) {
        const char *extension=strrchr(entry->d_name,'.'); if(!extension||strcmp(extension,".rword")) continue;
        char path[4096]; int n=snprintf(path,sizeof(path),"%s/%s",directory.c_str(),entry->d_name); if(n<=0||n>=int(sizeof(path))) continue;
        struct stat info{}; if(lstat(path,&info)<0||!S_ISREG(info.st_mode)||!info.st_size||info.st_uid!=getuid()) continue;
        if(lock_recovery_path(path,descriptor,error)) { if(!candidate.set(path)) unlock_recovery_path(descriptor); else break; }
    }
    closedir(dir); if(candidate.empty()) return;
    int choice=fl_choice("%s",T("Later"),T("Discard"),T("Recover"),T("An unsaved ReliefWord recovery copy is available. Recover it into a new document?"));
    if(choice==0) { unlock_recovery_path(descriptor); return; }
    if(choice==1) { remove_copy(candidate,descriptor); return; }
    Document recovered;
    if(!load_document(candidate.c_str(),FileFormat::Native,recovered,error)||!editor.replace_document(static_cast<Document &&>(recovered))) {
        unlock_recovery_path(descriptor); fl_alert("%s",T(error.empty()?editor.error.c_str():error.c_str())); return;
    }
    recovered_from_=candidate; recovered_descriptor_=descriptor; path_.clear(); stamp_={}; import_path_.clear(); import_stamp_={}; saved_revision_=0; statistics_revision_=0;
    view->reflow(true); changed(); message(T("Recovered — save this document to a new file"));
}
}
