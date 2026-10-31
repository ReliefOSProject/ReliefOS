#ifndef RELIEFOS_DISKMGR_ELEVATION_H
#define RELIEFOS_DISKMGR_ELEVATION_H
#include <sys/types.h>
#include <stddef.h>

namespace dm {
/** @brief Launch the installed Diskmgr through upstream sudo; passwords only travel on stdin. */
class Elevation {
    pid_t pid_ = -1;
    int input_ = -1;
    size_t size_ = 0;
    char output_[4096]{};
public:
    /** @brief Close an owned result descriptor; a successfully started root GUI is detached. */
    ~Elevation();
    /** @brief Start a fixed executable/argument vector; never interpret a shell command. */
    int start(const char *password);
    /** @brief Return0 while pending,1 on failure,2 after the real root GUI announces readiness. */
    int poll();
    /** @brief Report whether the authentication child still needs processing. */
    bool busy() const { return pid_ > 0; }
};
/** @brief Announce a displayed elevated GUI, then stop writing to the parent's authentication pipe. */
void elevated_ready();
}
#endif
