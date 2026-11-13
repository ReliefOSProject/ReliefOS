#ifndef RELIEFOS_CLIPBOARD_DATA_H
#define RELIEFOS_CLIPBOARD_DATA_H

#include "../reliefapk/storage.h"

namespace clip {
using ra::Array;
using ra::Text;
constexpr size_t MaxFormats = 64;
constexpr size_t MaxEntries = 100;
constexpr size_t MaxEntryBytes = 64 * 1024 * 1024;
constexpr size_t MaxHistoryBytes = 128 * 1024 * 1024;

struct Payload {
    Text target, type, bytes;
};

struct Entry {
    uint64_t id = 0, created = 0;
    Array<Payload> formats;
    size_t size() const;
    const Payload *find(const char *target) const;
    bool clone(const Entry &source);
    bool add(const char *target, const char *type, const void *bytes, size_t size);
};

bool same(const Entry &, const Entry &);
bool file_payload(const Array<Text> &paths, bool cut, Entry &out);
bool parse_files(const Text &bytes, bool gnome, Array<Text> &out, bool &cut);
bool local_uri(const char *uri, size_t length, Text &path);
Text preview(const Entry &);
bool matches(const Entry &, const char *query);
} // namespace clip
#endif
