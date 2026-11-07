#include "jobs.h"
#include <string.h>
namespace rm {
IoJob::~IoJob(){if(started_){cancel();pthread_join(thread_,nullptr);}}
bool IoJob::cancelled(void *p){return __atomic_load_n(&static_cast<IoJob*>(p)->cancelled_,__ATOMIC_ACQUIRE)!=0;}
void IoJob::progress(void *p,IoPhase phase,size_t done,size_t total){
    IoJob *job=static_cast<IoJob*>(p);
    __atomic_store_n(&job->phase_,int(phase),__ATOMIC_RELAXED);
    __atomic_store_n(&job->completed_,uint64_t(done),__ATOMIC_RELAXED);
    __atomic_store_n(&job->total_,uint64_t(total),__ATOMIC_RELAXED);
}
void IoJob::status(IoPhase &phase,uint64_t &done,uint64_t &total)const{
    phase=IoPhase(__atomic_load_n(&phase_,__ATOMIC_RELAXED));
    done=__atomic_load_n(&completed_,__ATOMIC_RELAXED);total=__atomic_load_n(&total_,__ATOMIC_RELAXED);
}
bool IoJob::launch(Error &error){
    __atomic_store_n(&done_,0,__ATOMIC_RELAXED);__atomic_store_n(&cancelled_,0,__ATOMIC_RELAXED);
    __atomic_store_n(&completed_,0,__ATOMIC_RELAXED);__atomic_store_n(&total_,0,__ATOMIC_RELAXED);
    if(pthread_create(&thread_,nullptr,entry,this)!=0){error.set(Code::Io,"Could not start background file task");return false;}
    started_=true;error.clear();return true;
}
bool IoJob::load(const char *path,uint64_t session,Error &error){
    if(started_){error.set(Code::Conflict,"A file task is already running");return false;}
    result_=IoResult{};result_.kind=IoKind::Load;result_.session=session;
    if(!result_.path.set(path)){error.set(Code::NoMemory,"Cannot store file path");return false;}
    return launch(error);
}
bool IoJob::save(const char *path,const Document &doc,const FileStamp &expected,bool capture,uint64_t session,Error &error){
    if(started_){error.set(Code::Conflict,"A file task is already running");return false;}
    result_=IoResult{};result_.kind=IoKind::Save;result_.session=session;
    if(!result_.path.set(path)||!clone_document(doc,result_.document,error)){if(error.code==Code::None)error.set(Code::NoMemory,"Cannot prepare save snapshot");return false;}
    expected_=expected;capture_existing_=capture;return launch(error);
}
bool IoJob::write(const char *path,Bytes &&bytes,bool overwrite,Error &error){
    if(started_){error.set(Code::Conflict,"A file task is already running");return false;}
    result_=IoResult{};result_.kind=IoKind::Write;
    if(!result_.path.set(path)){error.set(Code::NoMemory,"Cannot store output path");return false;}
    bytes_=static_cast<Bytes&&>(bytes);expected_=FileStamp{};capture_existing_=overwrite;return launch(error);
}
bool IoJob::recovery(IoKind kind,RecoveryStore &store,const Document *doc,const char *hint,uint64_t id,Error &error){
    if(started_){error.set(Code::Conflict,"A file task is already running");return false;}
    result_=IoResult{};result_.kind=kind;recovery_=&store;recovery_id_=id;
    if((hint&&!result_.path.set(hint))||(doc&&!clone_document(*doc,result_.document,error))){if(error.code==Code::None)error.set(Code::NoMemory,"Cannot prepare recovery snapshot");return false;}
    return launch(error);
}
void *IoJob::entry(void *p){
    IoJob &job=*static_cast<IoJob*>(p);IoResult &r=job.result_;
    IoControl control{cancelled,&job,progress};
    if(cancelled(&job))r.error.set(Code::Cancelled,"File task cancelled");
    else switch(r.kind){
    case IoKind::Load:r.success=read_document(r.path.c_str(),r.document,r.stamp,r.error,&control);break;
    case IoKind::Save:case IoKind::Write:{
        bool okay=!job.capture_existing_||file_stamp(r.path.c_str(),job.expected_,r.error,&control);
        if(okay)r.success=r.kind==IoKind::Save?
            atomic_save(r.path.c_str(),r.document,job.expected_,r.stamp,r.error,&control):
            atomic_save_bytes(r.path.c_str(),job.bytes_.data(),job.bytes_.size(),job.expected_,r.stamp,r.error,&control);
        break;
    }
    case IoKind::RecoveryInit:r.success=job.recovery_->open_default(r.error)&&job.recovery_->list(r.error);break;
    case IoKind::RecoverySave:r.success=job.recovery_->save(r.document,r.path.c_str(),r.error,&control);break;
    case IoKind::RecoveryLoad:r.success=job.recovery_->load(job.recovery_id_,r.document,r.error,&control);break;
    case IoKind::RecoveryDiscard:r.success=job.recovery_id_?job.recovery_->discard(job.recovery_id_,r.error):job.recovery_->discard_own(r.error);break;
    }
    __atomic_store_n(&job.done_,1,__ATOMIC_RELEASE);return nullptr;
}
bool IoJob::take(IoResult &result){
    if(!started_||!__atomic_load_n(&done_,__ATOMIC_ACQUIRE))return false;
    pthread_join(thread_,nullptr);started_=false;result=static_cast<IoResult&&>(result_);bytes_.clear();return true;
}
}
