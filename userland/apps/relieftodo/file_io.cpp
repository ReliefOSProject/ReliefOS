#include "file_io.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace rt {
namespace {
bool fail(Text &error, const char *operation) {
    char message[384]; snprintf(message,sizeof(message),"%s：%s",operation,strerror(errno));
    error.set(message); return false;
}
FileStamp from_stat(const struct stat &s, uint64_t digest=0) {
    FileStamp t; t.exists=true; t.device=s.st_dev; t.inode=s.st_ino; t.size=s.st_size;
    t.mtime=s.st_mtim.tv_sec; t.mtime_ns=s.st_mtim.tv_nsec; t.ctime=s.st_ctim.tv_sec; t.ctime_ns=s.st_ctim.tv_nsec;
    t.digest=digest; return t;
}
bool same_stat(const struct stat &a, const struct stat &b) { return same_stamp(from_stat(a),from_stat(b)); }
bool parent(const char *path, int &dir, Text &name, Text &error) {
    if (!path || !*path || strlen(path)>4095) { error.set("文件路径为空或过长"); return false; }
    Text copy; if (!copy.set(path)) { error.set("内存不足"); return false; }
    char *p=strdup(copy.c_str());
    if (!p) { error.set("内存不足"); return false; }
    dir=open(*p=='/'?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    bool ok=dir>=0;
    char *part=p; while (*part=='/') ++part;
    while (ok && *part) {
        char *end=strchr(part,'/');
        if (!end) { ok=strcmp(part,".") && strcmp(part,"..") && name.set(part); break; }
        *end=0;
        if (!strcmp(part,"..")) { error.set("文件路径不能包含上级目录"); ok=false; break; }
        if (*part && strcmp(part,".")) {
            int next=openat(dir,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if (next<0) { fail(error,"不能打开父目录（不支持符号链接）"); ok=false; break; }
            close(dir); dir=next;
        }
        part=end+1; while (*part=='/') ++part;
    }
    if (ok && name.empty()) { error.set("请选择一个文件名"); ok=false; }
    if (!ok) { if (error.empty()) fail(error,"无效的文件路径"); if (dir>=0) close(dir); dir=-1; }
    free(p); return ok;
}
bool read_at(int,const char *,Bytes &,FileStamp &,Text &,bool);
bool export_target_allowed(const char *path,const char *document,Text &error) {
    if (!document || !*document) return true;
    int target_dir=-1, document_dir=-1; Text target_name, document_name;
    if (!parent(path,target_dir,target_name,error)) return false;
    if (!parent(document,document_dir,document_name,error)) { close(target_dir); return false; }
    struct stat target_parent{}, document_parent{};
    bool ok=fstat(target_dir,&target_parent)==0 && fstat(document_dir,&document_parent)==0;
    if (!ok) fail(error,"不能核对导出目录");
    bool same=ok && target_parent.st_dev==document_parent.st_dev &&
        target_parent.st_ino==document_parent.st_ino && target_name==document_name;
    if (ok && !same) {
        FileStamp source, target; Bytes source_bytes, target_bytes;
        ok=read_at(document_dir,document_name.c_str(),source_bytes,source,error,true) &&
            read_at(target_dir,target_name.c_str(),target_bytes,target,error,true);
        // FAT/exFAT stat may synthesize an inode from the supplied path.
        // Comparing full content also catches case-folded aliases there;
        // conservatively protect an identical copy of the active document.
        same=ok && source.exists && target.exists &&
            ((source.device==target.device && source.inode==target.inode) ||
             (source_bytes.size()==target_bytes.size() &&
              (!source_bytes.size() || !memcmp(source_bytes.data(),target_bytes.data(),source_bytes.size()))));
    }
    close(document_dir); close(target_dir);
    if (same) { error.set("导出目标是当前任务清单，请选择其他文件"); return false; }
    return ok;
}
class SaveLock {
    int fd_=-1;
public:
    ~SaveLock() { if (fd_>=0) close(fd_); }
    bool acquire(int dir,const char *target,Text &error) {
        // Keep this inode after unlocking: removing a lock file lets a third
        // writer lock a new inode while another still holds the old one.
        fd_=openat(dir,".relieftodo-save.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
        if (fd_<0) return fail(error,"不能打开保存锁文件");
        struct stat metadata{};
        if (fstat(fd_,&metadata)<0) return fail(error,"不能检查保存锁文件");
        if (!S_ISREG(metadata.st_mode) || metadata.st_nlink!=1 || metadata.st_uid!=geteuid()) {
            error.set("保存锁必须是当前用户拥有的普通文件，且不能有其他硬链接"); return false;
        }
        if (flock(fd_,LOCK_EX|LOCK_NB)<0) {
            if (errno==EWOULDBLOCK || errno==EAGAIN) { error.set("另一个 ReliefTodo 正在保存此目录，请稍后重试"); return false; }
            return fail(error,"不能取得保存锁");
        }
        // FAT can expose an 8.3 alias for this lock file. Opening the target
        // through another descriptor and probing its lock detects that alias
        // using the filesystem's real lock identity, rather than stat's
        // potentially path-derived inode. Also respect other flock writers.
        int check=openat(dir,target,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if (check<0) return errno==ENOENT?true:fail(error,"不能检查保存目标");
        bool ok=flock(check,LOCK_EX|LOCK_NB)==0;
        if (!ok) error.set("目标文件已被锁定或属于保存锁，请重试或选择其他文件名");
        close(check); return ok;
    }
};
bool read_at(int dir, const char *name, Bytes &bytes, FileStamp &stamp, Text &error, bool missing_ok) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) {
        if (missing_ok && errno==ENOENT) { stamp=FileStamp{}; bytes.clear(); return true; }
        return fail(error,"不能打开文件");
    }
    struct stat before{}, after{}, named{}; bool ok=fstat(fd,&before)==0;
    if (!ok) fail(error,"不能读取文件信息");
    if (ok && (!S_ISREG(before.st_mode) || before.st_size<0 || uint64_t(before.st_size)>MaxFileBytes)) {
        error.set("只支持不超过 16 MiB 的普通文件"); ok=false;
    }
    Bytes buffer;
    if (ok && !buffer.resize(size_t(before.st_size))) { error.set("内存不足"); ok=false; }
    size_t at=0;
    while (ok && at<buffer.size()) {
        size_t count=buffer.size()-at; if (count>65536) count=65536;
        ssize_t n=::read(fd,buffer.data()+at,count);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { if (!n) error.set("文件已截断"); else fail(error,"读取失败"); ok=false; break; }
        at+=size_t(n);
    }
    if (ok && (fstat(fd,&after)<0 || fstatat(dir,name,&named,AT_SYMLINK_NOFOLLOW)<0 ||
        !S_ISREG(named.st_mode) || !same_stat(before,after) || !same_stat(after,named))) {
        error.set("文件在读取过程中被其他程序修改，请重试"); ok=false;
    }
    close(fd);
    if (!ok) return false;
    uint64_t digest=UINT64_C(14695981039346656037);
    for (size_t n=0;n<buffer.size();++n) { digest^=static_cast<unsigned char>(buffer[n]); digest*=UINT64_C(1099511628211); }
    stamp=from_stat(after,digest); bytes.swap(buffer); return true;
}
bool write_all(int fd,const char *bytes,size_t size) {
    size_t at=0;
    while (at<size) {
        size_t count=size-at; if (count>65536) count=65536;
        ssize_t n=::write(fd,bytes+at,count);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { if (!n) errno=EIO; return false; }
        at+=size_t(n);
    }
    return true;
}
struct Header { uint32_t magic=0x5254444f, ok=0, size=0; FileStamp stamp; char error[512]{}; };
}
bool same_stamp(const FileStamp &a,const FileStamp &b) {
    return a.exists==b.exists && (!a.exists || (a.device==b.device && a.inode==b.inode && a.size==b.size &&
        a.digest==b.digest && a.mtime==b.mtime && a.mtime_ns==b.mtime_ns && a.ctime==b.ctime && a.ctime_ns==b.ctime_ns));
}
bool read_file(const char *path,Bytes &bytes,FileStamp &stamp,Text &error) {
    int dir=-1; Text name; error.clear(); if (!parent(path,dir,name,error)) return false;
    bool ok=read_at(dir,name.c_str(),bytes,stamp,error,false); close(dir); return ok;
}
bool file_stamp(const char *path,FileStamp &stamp,Text &error) {
    int dir=-1; Text name; Bytes bytes; error.clear(); if (!parent(path,dir,name,error)) return false;
    bool ok=read_at(dir,name.c_str(),bytes,stamp,error,true); close(dir); return ok;
}
bool atomic_save(const char *path,const Bytes &bytes,const FileStamp &expected,FileStamp &out,Text &error) {
    if (bytes.size()>MaxFileBytes) { error.set("文件超出 16 MiB 限制"); return false; }
    int dir=-1; Text name; error.clear(); if (!parent(path,dir,name,error)) return false;
    if (!strcasecmp(name.c_str(),".relieftodo-save.lock")) {
        error.set("此文件名由 ReliefTodo 保存锁保留，请选择其他文件名"); close(dir); return false;
    }
    SaveLock lock;
    if (!lock.acquire(dir,name.c_str(),error)) { close(dir); return false; }
    Bytes unused; FileStamp actual;
    bool ok=read_at(dir,name.c_str(),unused,actual,error,true);
    if (ok && !same_stamp(actual,expected)) { error.set("文件已被其他程序修改；请重新打开或另存为，避免覆盖"); ok=false; }
    char temp[64]{}; int fd=-1;
    for (unsigned n=0;ok && n<128;++n) {
        snprintf(temp,sizeof(temp),".relieftodo-%ld-%u.tmp",long(getpid()),n);
        fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd>=0) break;
        if (errno!=EEXIST) { fail(error,"不能创建临时文件"); ok=false; }
    }
    if (ok && fd<0) { error.set("无法分配临时文件"); ok=false; }
    if (ok && (!write_all(fd,bytes.data(),bytes.size()) || fsync(fd)<0)) ok=fail(error,"写入或同步失败");
    if (fd>=0 && close(fd)<0 && ok) ok=fail(error,"关闭临时文件失败");
    bool renamed=false;
    if (ok) {
        ok=read_at(dir,name.c_str(),unused,actual,error,true);
        if (ok && !same_stamp(actual,expected)) { error.set("保存冲突：磁盘文件已更改，请另存为"); ok=false; }
    }
    if (ok) {
        if (renameat(dir,temp,dir,name.c_str())<0) ok=fail(error,"替换文件失败");
        else renamed=true;
    }
    if (ok && fsync(dir)<0) { error.set("文件已替换，但目录同步失败；请重新打开核对后再保存"); ok=false; }
    if (ok) ok=read_at(dir,name.c_str(),unused,out,error,false);
    if (fd>=0 && !renamed) unlinkat(dir,temp,0);
    close(dir); return ok;
}
bool IoJob::start(Kind kind,const char *path,const Bytes *bytes,const FileStamp *expected,Text &error,const char *protected_document) {
    if (busy()) { error.set("文件操作正在进行"); return false; }
    int pipefd[2]; if (pipe(pipefd)<0) return fail(error,"不能创建工作进程管道");
    int flags=fcntl(pipefd[0],F_GETFL);
    if (flags<0 || fcntl(pipefd[0],F_SETFL,flags|O_NONBLOCK)<0) {
        close(pipefd[0]); close(pipefd[1]); return fail(error,"不能设置非阻塞管道");
    }
    pid_t child=fork();
    if (child<0) { close(pipefd[0]); close(pipefd[1]); return fail(error,"不能启动文件工作进程"); }
    if (!child) {
        close(pipefd[0]); signal(SIGPIPE,SIG_IGN);
        Header header; Bytes loaded; Text reason;
        bool ok=export_target_allowed(path,protected_document,reason);
        if (ok) ok=kind==Kind::Read?read_file(path,loaded,header.stamp,reason):
            kind==Kind::Probe?file_stamp(path,header.stamp,reason):atomic_save(path,*bytes,*expected,header.stamp,reason);
        header.ok=ok?1:0; header.size=ok?uint32_t(loaded.size()):0;
        snprintf(header.error,sizeof(header.error),"%s",reason.c_str());
        bool sent=write_all(pipefd[1],reinterpret_cast<const char *>(&header),sizeof(header)) &&
            (!header.size || write_all(pipefd[1],loaded.data(),loaded.size()));
        close(pipefd[1]); _exit(sent?0:1);
    }
    close(pipefd[1]); fd_=pipefd[0]; pid_=child; kind_=kind; eof_=finished_=false;
    received_.clear(); reply_=Reply{}; error.clear(); return true;
}
bool IoJob::read(const char *path,Text &error) { return start(Kind::Read,path,nullptr,nullptr,error); }
bool IoJob::probe(const char *path,Text &error,const char *protected_document) { return start(Kind::Probe,path,nullptr,nullptr,error,protected_document); }
bool IoJob::write(const char *path,const Bytes &bytes,const FileStamp &expected,Text &error,const char *protected_document) { return start(Kind::Write,path,&bytes,&expected,error,protected_document); }
bool IoJob::poll() {
    if (!busy()) return false;
    if (finished_) return true;
    char chunk[8192]; size_t budget=256*1024;
    while (!eof_ && budget) {
        ssize_t n=::read(fd_,chunk,sizeof(chunk));
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) break;
        if (n<=0) { eof_=true; if (n<0) fail(reply_.error,"工作进程读取失败"); break; }
        size_t at=received_.size(); budget-=size_t(n);
        if (at+size_t(n)>MaxFileBytes+sizeof(Header) || !received_.resize(at+size_t(n))) {
            reply_.error.set("工作进程响应过大或内存不足"); eof_=true; break;
        }
        memcpy(received_.data()+at,chunk,size_t(n));
    }
    if (!eof_) return false;
    if (fd_>=0) { close(fd_); fd_=-1; }
    int status=0; pid_t p=waitpid(pid_,&status,WNOHANG);
    if (!p || (p<0 && errno==EINTR)) return false;
    pid_=-1; finished_=true;
    if (!reply_.error.empty()) return true;
    if (p<0 || !WIFEXITED(status) || WEXITSTATUS(status) || received_.size()<sizeof(Header)) {
        reply_.error.set("文件工作进程意外退出，原文档仍保留在窗口中"); return true;
    }
    Header header; memcpy(&header,received_.data(),sizeof(header)); header.error[sizeof(header.error)-1]=0;
    if (header.magic!=0x5254444f || header.size>MaxFileBytes || received_.size()!=sizeof(header)+header.size) {
        reply_.error.set("文件工作进程响应无效"); return true;
    }
    reply_.ok=header.ok==1; reply_.stamp=header.stamp; reply_.error.set(header.error);
    if (reply_.ok && header.size) {
        if (!reply_.bytes.resize(header.size)) { reply_.ok=false; reply_.error.set("内存不足"); }
        else memcpy(reply_.bytes.data(),received_.data()+sizeof(header),header.size);
    }
    return true;
}
IoJob::Reply IoJob::take() {
    Reply result=static_cast<Reply &&>(reply_); received_.clear(); kind_=Kind::Idle; finished_=false; return result;
}
IoJob::~IoJob() {
    if (fd_>=0) close(fd_);
    // Normal close waits for an operation to finish; never interrupt a commit.
    if (pid_>0) waitpid(pid_,nullptr,WNOHANG);
}
}
