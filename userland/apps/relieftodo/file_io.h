#ifndef RELIEFTODO_FILE_IO_H
#define RELIEFTODO_FILE_IO_H
#include "document.h"
#include <sys/types.h>
namespace rt {
struct FileStamp {
    bool exists = false;
    uint64_t device=0, inode=0, size=0, digest=0;
    int64_t mtime=0, mtime_ns=0, ctime=0, ctime_ns=0;
};
bool same_stamp(const FileStamp &, const FileStamp &);
bool read_file(const char *, Bytes &, FileStamp &, Text &error);
bool file_stamp(const char *, FileStamp &, Text &error);
bool atomic_save(const char *, const Bytes &, const FileStamp &, FileStamp &, Text &error);
// Only the child touches the disk. The parent polls a nonblocking reply pipe
// with a bounded read budget; a completed reply is consumed with take().
class IoJob {
public:
    enum class Kind { Idle, Read, Probe, Write };
    struct Reply { bool ok=false; FileStamp stamp; Bytes bytes; Text error; };
    bool read(const char *, Text &error);
    // Export callers provide the active document path. Both destination probe
    // and write reject aliases of that location or its current file identity.
    bool probe(const char *, Text &error, const char *protected_document=nullptr);
    bool write(const char *, const Bytes &, const FileStamp &, Text &error, const char *protected_document=nullptr);
    bool poll();
    bool busy() const { return kind_!=Kind::Idle; }
    Kind kind() const { return kind_; }
    Reply take();
    ~IoJob();
private:
    Kind kind_=Kind::Idle;
    pid_t pid_=-1;
    int fd_=-1;
    bool eof_=false, finished_=false;
    Bytes received_;
    Reply reply_;
    bool start(Kind, const char *, const Bytes *, const FileStamp *, Text &error, const char *protected_document=nullptr);
};
}
#endif
