#ifndef RELIEFPPT_DOCUMENT_H
#define RELIEFPPT_DOCUMENT_H

#include "model.h"

namespace rp {

struct FileStamp {
    bool exists = false;
    uint64_t device = 0, inode = 0, size = 0;
    int64_t seconds = 0, nanoseconds = 0, change_seconds = 0, change_nanoseconds = 0;
};

bool file_stamp(const char *path, FileStamp &stamp, Text &error);
bool same_stamp(const FileStamp &a, const FileStamp &b);
bool encode_deck(const Deck &deck, Text &bytes, Text &error);
bool decode_deck(const char *bytes, size_t length, Deck &out, Text &error);
bool load_deck(const char *path, Deck &out, Text &error);
bool save_deck(const char *path, const Deck &deck, Text &error,
    const FileStamp *expected = nullptr);
bool atomic_write(const char *path, const char *bytes, size_t length, Text &error,
    const FileStamp *expected = nullptr);
bool recovery_directory(Text &output, Text &error);
bool reserve_recovery_path(Text &output, Text &error);
bool lock_recovery_path(const char *path, int &descriptor, Text &error);
bool find_latest_recovery(Text &path, int &descriptor, Deck &deck, Text &error);
void unlock_recovery_path(int &descriptor);

}

#endif
