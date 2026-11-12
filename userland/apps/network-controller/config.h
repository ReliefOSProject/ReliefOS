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
  const char *interface_;
  int profile_directory(bool create, Text &error);
  void profile_names(char (&name)[32], char (&backup)[40]) const;
  bool replace(const Snapshot &, const Snapshot &, Text &);

public:
  explicit ConfigStore(const char *directory = "/etc/reliefos",
                       const char *interface = "eth0")
      : directory_(directory), interface_(interface) {}
  bool read(Snapshot &, Text &);
  bool save(const Snapshot &, const Text &, Text &);
  bool restore(const Snapshot &, Text &);
};
} // namespace nc
#endif
