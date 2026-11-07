#ifndef RELIEFMATH_EXPORT_H
#define RELIEFMATH_EXPORT_H
#include "jobs.h"
#include "process.h"
namespace rm {
enum class ExportFormat { Svg,Png,Latex };
enum class ExportStatus { Pending,Rendering,Writing,Done,Failed,Cancelled };
struct ExportOptions {Text directory,pattern;ExportFormat format=ExportFormat::Svg;double scale=1;uint32_t dpi=96;bool overwrite=false;};
struct ExportItem {Snapshot snapshot;Text path;ExportStatus status=ExportStatus::Pending;Error error;};
bool export_filename(const char *,const char *,const char *,size_t,ExportFormat,Text &,Error &);
/** Sequential immutable snapshots; each output is committed by its own atomic I/O job. */
class ExportQueue {
    Array<ExportItem> items_;
    RenderClient renderer_;
    IoJob writer_;
    ExportOptions options_;
    size_t cursor_=0;
    uint64_t next_request_=0;
    bool active_=false,cancelled_=false;
    void next();
    void write(Bytes &&);
public:
    bool start(const char *,const Document &,const FormulaId *,size_t,uint64_t,const ExportOptions &,Error &);
    /** Nonblocking progress and child reap; call every GUI tick even when busy() is false. */
    void pump();
    void cancel();
    bool busy()const{return active_;}
    const Array<ExportItem> &items()const{return items_;}
    size_t position()const{return cursor_;}
};
}
#endif
