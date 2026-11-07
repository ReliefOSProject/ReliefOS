#ifndef RELIEFAPK_MODEL_H
#define RELIEFAPK_MODEL_H

#include "storage.h"

namespace ra {
struct Package {
    Text name, version, description, arch, license, url, origin;
    Text depends, reverse_depends, provides, repositories, contents;
    uint64_t installed_size = 0, file_size = 0;
    bool installed = false;
};

class PackageParser {
    struct State;
    State *state_ = nullptr;
    Text fallback_error_;
public:
    PackageParser() = default;
    ~PackageParser();
    PackageParser(const PackageParser &) = delete;
    PackageParser &operator=(const PackageParser &) = delete;
    bool begin(const char *json, size_t length);
    bool step(unsigned budget = 128);
    bool done() const;
    const Text &error() const;
    Array<Package> take();
};

struct Settings {
    bool offline = false, fetch_dependencies = false;
    unsigned timeout = 30, cache_max_age = 240;
    Text cache_dir;
};
enum class Action {
    Install, Remove, Upgrade, Reinstall, Fix, UpgradeAll, Update, CacheClean,
    CacheDownload, Fetch, Verify, Audit, Stats, Policy, Contents, Depends,
    ReverseDepends, Owner, World, Version, Dot, Manifest, Keys, RepairCa
};
constexpr const char *WorkerPath = "/usr/lib/reliefos/apps/reliefapk/reliefapk.elf";
constexpr const char *CertificatePackage = "ca-certificates-bundle";
constexpr const char *CertificateSourcesPackage = "ca-certificates";
constexpr const char *BootstrapCaBundle = "/usr/share/reliefos/certs/ca-certificates.crt";
struct Command { Array<Text> args; bool privileged = false, mutating = false, cancelable = true; };

bool make_command(Action action, const Array<Text> &targets, const Settings &settings,
                  bool simulate, Command &command, Text &error);
bool certificate_apk_command(bool reinstall, bool simulate, Command &command, Text &error);
bool query_command(int filter, const Settings &settings, Command &command, Text &error);
bool metadata_command(const char *name, const Settings &settings, Command &command, Text &error);
bool valid_constraint(const char *value, bool local = false);
Text format_size(uint64_t bytes);
}

#endif
