#ifndef RELIEFOS_DISKMGR_WORKER_H
#define RELIEFOS_DISKMGR_WORKER_H
#include "model.h"
#include <sys/types.h>

namespace dm {
/** @brief Own one child and its bounded reply pipe; GUI code never waits synchronously. */
class Worker {
    pid_t pid_ = -1;
    int input_ = -1;
    size_t received_ = 0;
    int transport_error_ = 0;
    bool eof_ = false;
    Reply reply_{};
    /** @brief Drain all currently available reply bytes, retaining short-read/error state. */
    void drain();
public:
    /** @brief Close only owned descriptors; callers keep the window alive while busy. */
    ~Worker();
    /** @brief Fork one immutable request; return negative errno on startup failure. */
    int start(const Request &request);
    /** @brief Drain nonblocking output and reap the child; return1 only for a completed job. */
    int poll();
    /** @brief Report whether an owned child still needs its completion/reap pass. */
    bool busy() const { return pid_ > 0; }
    /** @brief Borrow the last reply until the next start. */
    const Reply &reply() const { return reply_; }
};
}
#endif
