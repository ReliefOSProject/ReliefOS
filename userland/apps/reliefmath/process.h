#ifndef RELIEFMATH_PROCESS_H
#define RELIEFMATH_PROCESS_H

#include "render_types.h"

namespace rm {

/**
 * @brief Owns one renderer worker and advances its request/result pipes without waiting.
 * The caller invokes pump() from its GUI event/timer path and receives at most one
 * completion through take(); submitted snapshots are retained until completion.
 */
class RenderClient {
    void *state_ = nullptr;
    RenderClient(const RenderClient &) = delete;
    RenderClient &operator=(const RenderClient &) = delete;

public:
    RenderClient() = default;
    ~RenderClient();

    /**
     * @brief Launches the worker without waiting for it to process a request.
     * @param worker_path NUL-terminated executable path, copied by this object.
     * @param error Receives a validation, allocation, pipe, or process-launch error.
     * @return True when the worker is launched or an existing worker already uses this path.
     */
    bool start(const char *worker_path, Error &error) noexcept;

    /**
     * @brief Replaces any in-flight request with an immutable copy of a new snapshot.
     * @param snapshot Render settings and UTF-8 source retained for this request.
     * @param error Receives validation, allocation, or worker setup errors.
     * @return True when the request is accepted for asynchronous delivery.
     */
    bool submit(const Snapshot &snapshot, Error &error) noexcept;

    /**
     * @brief Performs a bounded nonblocking read/write, timeout check, and child reap.
     * Call repeatedly from the GUI event loop; it never waits for I/O or process exit.
     */
    void pump() noexcept;

    /**
     * @brief Transfers one completed render or failure to the caller.
     * @param rendered Receives the result identity and any successfully assembled artifacts.
     * @param error Receives the worker failure, or is cleared on success.
     * @return True if a success or failure was available; false if no result is ready.
     */
    bool take(Rendered &rendered, Error &error) noexcept;

    /** @brief Reports whether a request is active, pending replacement, or awaiting its worker. */
    bool busy() const noexcept;

    /** @brief Cancels the active request and asynchronously reaps only this object's child. */
    void cancel() noexcept;
};

} // namespace rm

#endif
