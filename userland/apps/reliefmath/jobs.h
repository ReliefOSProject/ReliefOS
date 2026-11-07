#ifndef RELIEFMATH_JOBS_H
#define RELIEFMATH_JOBS_H
#include "file_io.h"
#include "recovery.h"
#include <pthread.h>
namespace rm {
enum class IoKind { Load, Save, Write, RecoveryInit, RecoverySave, RecoveryLoad, RecoveryDiscard };
struct IoResult {
    IoKind kind=IoKind::Load;
    bool success=false;
    uint64_t session=0;
    Text path;
    Document document;
    FileStamp stamp;
    Error error;
};
/** One owned background I/O thread. Only take() publishes its temporary state. */
class IoJob {
    pthread_t thread_{};
    bool started_=false;
    int done_=0,cancelled_=0,phase_=0;
    uint64_t completed_=0,total_=0;
    IoResult result_;
    Bytes bytes_;
    FileStamp expected_;
    bool capture_existing_=false;
    RecoveryStore *recovery_=nullptr;
    uint64_t recovery_id_=0;
    static void *entry(void *);
    static bool cancelled(void *);
    static void progress(void *,IoPhase,size_t,size_t);
    bool launch(Error &);
public:
    IoJob()=default;
    ~IoJob();
    IoJob(const IoJob&)=delete;
    IoJob& operator=(const IoJob&)=delete;
    bool load(const char *,uint64_t,Error &);
    bool save(const char *,const Document &,const FileStamp &,bool,uint64_t,Error &);
    bool write(const char *,Bytes &&,bool,Error &);
    bool recovery(IoKind,RecoveryStore &,const Document *,const char *,uint64_t,Error &);
    bool busy()const {return started_&&!__atomic_load_n(&done_,__ATOMIC_ACQUIRE);}
    bool pending()const{return started_;}
    bool take(IoResult &);
    void cancel(){__atomic_store_n(&cancelled_,1,__ATOMIC_RELEASE);}
    void status(IoPhase &,uint64_t &,uint64_t &)const;
};
}
#endif
