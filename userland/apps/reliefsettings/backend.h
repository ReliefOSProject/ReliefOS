#ifndef RELIEFSETTINGS_BACKEND_H
#define RELIEFSETTINGS_BACKEND_H
#include "../reliefapk/process.h"
#include "config.h"
#include <sys/types.h>

namespace rs {
constexpr const char *Program = "/usr/lib/reliefos/apps/reliefsettings/reliefsettings.elf";
double seconds();
bool command(const char *const argv[], Text &output, Text &error);
bool apply_mode(const DisplaySetting &, Text &error);
int apply_session();
int session_environment();
bool load_zones(Array<Text> &, Text &error);
Text current_timezone();

// Workers only access their fork-private data. The GUI pumps bounded replies
// while configuration fsync and external X11 clients run in the child.
struct Reply {
    char status = 0;
    char error[1024]{};
};
class Job {
    pid_t pid_ = -1;
    int socket_ = -1;
    Reply pending_{};
    size_t received_ = 0;

  public:
    ~Job();
    bool start(void (*work)(int, void *), void *context, int display_fd);
    bool poll(Reply &);
    bool decide(bool keep);
    void close();
    bool active() const {
        return socket_ >= 0;
    }
};
struct SaveRequest {
    const char *directory;
    const char *name;
    const Snapshot *expected;
    const Text *bytes;
};
struct KeyboardRequest {
    SaveRequest save;
    const char *layout;
    const char *variant;
};
struct DisplayRequest {
    SaveRequest save;
    const DisplaySetting *after;
};
void save_worker(int channel, void *context);
void keyboard_worker(int channel, void *context);
void display_worker(int channel, void *context);
} // namespace rs
#endif
