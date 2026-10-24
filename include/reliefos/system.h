#ifndef RELIEFOS_SYSTEM_H
#define RELIEFOS_SYSTEM_H

/*
 * Userland system client API. The wire types and constants moved to the
 * kernel UAPI (<reliefos/system_abi.h>); this header re-exports them so existing
 * `#include <reliefos/system.h>` callers keep working.
 */
#include <reliefos/system_abi.h>
#include <reliefos/kernel_debug.h>
#include <stdint.h>

int reliefos_system_info(struct reliefos_system_info *info);
int reliefos_perf_info(struct reliefos_perf_info *info);
int reliefos_task_affinity_get(uint32_t pid, uint64_t *mask);
int reliefos_task_affinity_set(uint32_t pid, uint64_t mask);
int reliefos_time_info(struct reliefos_time_info *info);
int reliefos_time_ntp_sync(uint32_t timeout_ms, struct reliefos_time_sync *result);
int reliefos_machine_identity(struct reliefos_machine_identity *identity);
int reliefos_system_reboot(void);
int reliefos_system_shutdown(void);

#define RELIEFOS_TASK_NAME_LEN 32U
#define RELIEFOS_TASK_MAX 64U

#define RELIEFOS_TASK_SNAPSHOT_FLAG_ELEVATED_ADMIN 0x00000010U

struct reliefos_task_info {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t kind;
    uint32_t flags;
    uint32_t uid;
    uint32_t role;
    uint32_t session_id;
    uint32_t memory_kib;
    uint64_t cpu_ticks;
    int32_t priority;
    uint32_t pending_signals;
    uint64_t wake_tick;
    uint64_t entry;
    uint64_t cr3;
    uint64_t affinity_mask;
    char name[RELIEFOS_TASK_NAME_LEN];
    char username[32];
};

struct reliefos_task_snapshot {
    uint32_t capacity;
    uint32_t count;
    uint64_t tick;
    struct reliefos_task_info *tasks;
};

unsigned long reliefos_uptime_ms(void);
int reliefos_task_snapshot(struct reliefos_task_info *tasks, uint32_t capacity, uint64_t *tick);
int reliefos_task_kill(uint32_t pid);

#endif
