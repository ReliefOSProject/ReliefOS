#ifndef RELIEFOS_TASKMGR_AUTOSTART_H
#define RELIEFOS_TASKMGR_AUTOSTART_H

#include "../reliefapk/storage.h"
#include <sys/types.h>

namespace autostart {
using ra::Array;
using ra::Text;
constexpr size_t MaxEntries = 2048;
constexpr size_t MaxFileBytes = 1024 * 1024;
constexpr size_t MaxSnapshotBytes = 16 * 1024 * 1024;

enum class State { Ready, Disabled, OtherDesktop, MissingProgram, Invalid };
struct Paths {
    Text personal, desktop;
    Array<Text> directories;
    bool initialize(Text &error);
};
struct Entry {
    Text id, source, name, command, comment, directory, try_exec, only_show, not_show, icon, data;
    bool personal = false, fallback = false, hidden = false, terminal = false, dbus = false;
    State state = State::Invalid;
};
struct Edit {
    Text name, command, comment, directory, try_exec;
    bool terminal = false, enabled = true;
};
enum class Action { Scan, Create, Edit, Toggle, Remove };
struct Request {
    Action action = Action::Scan;
    uid_t uid = 0;
    Entry original;
    Edit edit;
};
struct Snapshot {
    Array<Entry> entries;
    Text error, selected;
};

bool scan(const Paths &, Snapshot &);
bool change(const Paths &, Request &, Text &selected, Text &error);
bool valid_command(const char *, Text &error);
Text executable_command(const char *path);
bool switch_user(uid_t, Text &error);
bool encode(const Snapshot &, Text &);
bool decode(const Text &, Snapshot &);
int run_session();
} // namespace autostart
#endif
