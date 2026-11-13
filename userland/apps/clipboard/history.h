#ifndef RELIEFOS_CLIPBOARD_HISTORY_H
#define RELIEFOS_CLIPBOARD_HISTORY_H
#include "data.h"

namespace clip {
class History {
    uint64_t next_ = 1;
public:
    Array<Entry> entries;
    uint64_t revision = 0;
    bool add(Entry &&entry);
    void remove(size_t index);
    void clear();
};

struct StatePaths {
    Text state, config;
    bool initialize();
};

int load_history(const char *directory, History &out);
int save_history(const char *directory, const History &history);
int load_preferences(const char *directory, bool &persist);
int save_preferences(const char *directory, bool persist);
int delete_history(const char *directory);
} // namespace clip
#endif
