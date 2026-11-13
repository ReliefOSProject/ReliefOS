#ifndef RELIEFSETTINGS_CONFIG_H
#define RELIEFSETTINGS_CONFIG_H
#include "model.h"
namespace rs {
struct Snapshot {
    bool exists = false;
    Text bytes;
};
class Store {
    const char *directory_;
    int open_directory(bool create, Text &error);

  public:
    explicit Store(const char *directory) : directory_(directory) {}
    bool read(const char *name, Snapshot &, Text &);
    bool save(const char *name, const Snapshot &, const Text &, Text &, bool *published = nullptr);
    int lock_desktop(Text &);
};
bool set_timezone(const char *zone, Text &error, const char *etc = "/etc",
                  const char *zoneinfo = "/usr/share/zoneinfo", bool *published = nullptr);
} // namespace rs
#endif
