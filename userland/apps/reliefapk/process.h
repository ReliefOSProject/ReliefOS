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
    /* Consume parsed stream prefixes so long-running clients can retain a
     * bounded window without discarding newer records at the capture limit. */
    void consume_output(size_t count);
    void consume_errors(size_t count);
    bool truncated() const;
    bool password_requested() const;
    bool provide_password(const char *secret);
    void cancel_auth();
    bool cancel();
    /* Ask only the worker leader to stop cooperatively. Unlike cancel(), this
     * never kills its process group or skips the worker's cleanup protocol. */
    bool request_stop();

private:
    void *state_;
};

} // namespace ra

#endif
