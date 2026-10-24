#ifndef RELIEFOS_TASKMGR_MODEL_H
#define RELIEFOS_TASKMGR_MODEL_H

/*
 * Toolkit-independent task manager behavior: formatting, utilization math,
 * the performance history ring and process tree ordering.  The Motif
 * frontend maps its widgets onto these; the host suite runs them bare.
 */
#include <stdint.h>

#define TASKMGR_PERF_HISTORY 120U
#define TASKMGR_PERF_MISSING 255U
#define TASKMGR_MAX_CORES 64U

struct taskmgr_perf_history {
    uint8_t memory[TASKMGR_PERF_HISTORY];
    uint8_t gpu[TASKMGR_PERF_HISTORY];
    uint8_t core[TASKMGR_MAX_CORES][TASKMGR_PERF_HISTORY];
    uint32_t head;
    uint32_t count;
};

struct taskmgr_tree_row {
    uint32_t index;
    uint32_t depth;
};

void taskmgr_format_percent(char *buf, uint32_t cap, uint32_t value);
void taskmgr_format_memory(char *buf, uint32_t cap, uint32_t kib);
void taskmgr_format_kib(char *buf, uint32_t cap, uint64_t kib);
void taskmgr_format_uptime(char *buf, uint32_t cap, uint64_t ms);
void taskmgr_format_hex_fixed(char *buf, uint32_t cap, uint64_t value, uint32_t digits);
void taskmgr_format_dec(char *buf, uint32_t cap, uint64_t value);

const char *taskmgr_state_name(uint32_t state);
const char *taskmgr_kind_name(uint32_t kind);

uint32_t taskmgr_task_cpu_percent(uint64_t used_ticks, uint64_t tick_delta,
                                  uint32_t fallback_percent, int have_fallback);
uint32_t taskmgr_busy_percent(uint64_t previous_busy, uint64_t previous_idle,
                              uint64_t busy, uint64_t idle,
                              int have_previous, uint32_t fallback_percent);
uint32_t taskmgr_mem_percent(uint64_t total_kib, uint64_t free_kib);
int taskmgr_task_killable(uint32_t pid, uint32_t self_pid, uint32_t kind,
                          uint32_t state, uint32_t flags);

void taskmgr_perf_history_init(struct taskmgr_perf_history *history);
uint32_t taskmgr_perf_history_push(struct taskmgr_perf_history *history,
                                   uint32_t memory_percent,
                                   uint32_t gpu_percent, int gpu_valid);
void taskmgr_perf_history_clear_gpu(struct taskmgr_perf_history *history);

uint32_t taskmgr_tree_order(const uint32_t *pids, const uint32_t *parents,
                            uint32_t count, struct taskmgr_tree_row *rows,
                            uint32_t capacity);

#endif
