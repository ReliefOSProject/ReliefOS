#ifndef RELIEFOS_INSTALLER_UPDATE_SUPERVISOR_H
#define RELIEFOS_INSTALLER_UPDATE_SUPERVISOR_H
#include <signal.h>
#include <stdint.h>

/* Run one blocking operation in an owned process group. The coordinator never
 * runs storage code while waiting. timeout is a hard monotonic stage deadline;
 * pulses report coordinator liveness and never extend it. A terminating group
 * is reaped before this function returns, including on cancellation or fault. */
typedef int (*installer_update_task_fn)(void *);
typedef void (*installer_update_pulse_fn)(void *, int stopping_reason);
int installer_update_supervise(installer_update_task_fn task, void *context,
    uint64_t timeout_ms, const volatile sig_atomic_t *cancelled,
    installer_update_pulse_fn pulse, void *pulse_context);
#endif
