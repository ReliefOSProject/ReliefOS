#ifndef OPENRC_CONTROLLER_CONFIG_H
#define OPENRC_CONTROLLER_CONFIG_H
#include "model.h"
#include <sys/stat.h>
namespace oc {
struct Snapshot {
  Text bytes;
  bool exists = false;
};
class ConfigStore {
  int root_ = -1;
  bool save_with_permissions(const char *, const Snapshot &, const Text &,
                             Text &, const struct stat *);

public:
  explicit ConfigStore(const char *etc = "/etc");
  ~ConfigStore();
  bool read(const char *name, Snapshot &, Text &error, bool backup = false);
  bool save(const char *name, const Snapshot &expected, const Text &bytes,
            Text &error);
  bool restore(const char *name, const Snapshot &expected, Text &error);
};
} // namespace oc
#endif
