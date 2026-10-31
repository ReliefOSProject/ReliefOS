#ifndef NETWORK_CONTROLLER_CONFIG_H
#define NETWORK_CONTROLLER_CONFIG_H
#include "model.h"
namespace nc {
struct Snapshot {
  bool exists = false;
  Text bytes;
};
class ConfigStore {
  const char *directory_;
  bool replace(const Snapshot &, const Snapshot &, Text &);

public:
  explicit ConfigStore(const char *directory = "/etc/reliefos")
      : directory_(directory) {}
  bool read(Snapshot &, Text &);
  bool save(const Snapshot &, const Text &, Text &);
  bool restore(const Snapshot &, Text &);
};
} // namespace nc
#endif
