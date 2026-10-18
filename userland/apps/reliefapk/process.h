#ifndef RELIEFAPK_PROCESS_H
#define RELIEFAPK_PROCESS_H

#include <stddef.h>

namespace ra {

enum class AuthMode { Sudo, RootPassword };

class Process {
public:
    Process();
    ~Process();
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    bool start(const char *const argv[], bool privileged = false,
        AuthMode mode = AuthMode::Sudo, bool cancelable = true);
    bool step(unsigned budget = 65536);
    bool running() const;
    int exit_code() const;
    int exec_error() const;
    const char *output() const;
    size_t output_size() const;
    const char *errors() const;
    size_t errors_size() const;
    bool truncated() const;
    bool password_requested() const;
    bool provide_password(const char *secret);
    void cancel_auth();
    bool cancel();

private:
    void *state_;
};

} // namespace ra

#endif
