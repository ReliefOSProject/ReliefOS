#ifndef RELIEFEXCEL_DOCUMENT_H
#define RELIEFEXCEL_DOCUMENT_H
#include "model.h"
namespace rx {
struct FileStamp {
    bool exists = false;
    uint64_t device = 0, inode = 0, size = 0;
    int64_t seconds = 0, nanoseconds = 0;
    int64_t change_seconds = 0, change_nanoseconds = 0;
};
bool file_stamp(const char *path, FileStamp &stamp, Text &error);
bool same_stamp(const FileStamp &a, const FileStamp &b);
bool load_workbook(const char *path, Workbook &output, Text &error, Progress progress = nullptr, void *context = nullptr);
bool save_workbook(const char *path, const Workbook &book, Text &error, const FileStamp *expected = nullptr, Progress progress = nullptr, void *context = nullptr);
bool import_csv(const char *path, Workbook &output, Text &error, char delimiter = ',', Progress progress = nullptr, void *context = nullptr);
bool export_csv(const char *path, Workbook &book, uint32_t sheet, Text &error, bool bom = true, Progress progress = nullptr, void *context = nullptr, const FileStamp *expected = nullptr);
// TSV for the system clipboard, same quoting rules as CSV with tabs as separators.
bool decode_delimited(const char *bytes, size_t length, Workbook &output, Text &error, char delimiter = ',');
bool encode_delimited(Workbook &book, uint32_t sheet, Range range, Text &output, Text &error, char delimiter = '\t');
bool recovery_directory(Text &output, Text &error);
bool reserve_recovery_path(Text &output, Text &error);
bool lock_recovery_path(const char *path, int &descriptor, Text &error);
void unlock_recovery_path(int &descriptor);
}
#endif
