#ifndef RELIEFOS_FILEMAN_VOLUMES_H
#define RELIEFOS_FILEMAN_VOLUMES_H

#include "../reliefapk/storage.h"
#include <stdint.h>

namespace fm {
using ra::Array;
using ra::Text;
constexpr const char *Program = "/usr/lib/reliefos/apps/fileman/fileman.elf";

struct Volume {
    Text name, disk, mount, filesystem;
    uint64_t sequence = 0, device = 0, bytes = 0;
    unsigned mounts = 0;
    bool readonly = false, system = false;
};

bool device_name(const char *name);
bool number(const char *text, uint64_t &value);
bool path_under(const char *path, const char *directory);
bool same_volume(const Volume &, const Volume &);
bool parse_mountinfo(const char *, uint64_t &device, Text &path, Text &type,
                     uint64_t *mount_id = nullptr, uint64_t *parent_id = nullptr);
bool mount_before(unsigned depth, uint64_t id, unsigned other_depth, uint64_t other_id);
int scan_volumes(Array<Volume> &, bool include_disks = false);
void print_volumes(const Array<Volume> &);
bool parse_volumes(const char *bytes, size_t size, Array<Volume> &);
int parse_volume_reply(const char *bytes, size_t size, Text &mount, Text &users);
// Return -1 for ordinary GUI arguments. Helpers run before initializing X11.
int volume_command(int argc, char **argv);
} // namespace fm
#endif
