#ifndef RELIEFWORD_DOCUMENT_H
#define RELIEFWORD_DOCUMENT_H
#include "model.h"
namespace rw {
enum class FileFormat { Native,Text,Html };
struct FileStamp {
    bool exists=false;
    uint64_t device=0,inode=0,size=0;
    int64_t seconds=0,nanoseconds=0,change_seconds=0,change_nanoseconds=0;
};
bool file_stamp(const char *path,FileStamp &stamp,Text &error);
bool same_stamp(const FileStamp &a,const FileStamp &b);
bool decode_document(const char *bytes,size_t length,FileFormat format,Document &output,Text &error,Text *warning=nullptr);
bool encode_document(const Document &doc,FileFormat format,Text &output,Text &error);
bool load_document(const char *path,FileFormat format,Document &output,Text &error,Text *warning=nullptr,Progress progress=nullptr,void *context=nullptr);
bool save_document(const char *path,const Document &doc,FileFormat format,Text &error,const FileStamp *expected=nullptr,Progress progress=nullptr,void *context=nullptr);
bool recovery_directory(Text &output,Text &error);
bool reserve_recovery_path(Text &output,Text &error);
bool lock_recovery_path(const char *path,int &descriptor,Text &error);
void unlock_recovery_path(int &descriptor);
}
#endif
