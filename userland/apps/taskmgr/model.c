#include "model.h"

static void append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (*pos + 1 < cap) {
        buf[(*pos)++] = ch;
        buf[*pos] = 0;
    }
}

static void append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text)
{
    for (uint32_t i = 0; text && text[i]; ++i) {
        append_char(buf, pos, cap, text[i]);
    }
}

void taskmgr_format_dec(char *buf, uint32_t cap, uint64_t value)
{
    char tmp[20];
    uint32_t n = 0;
    uint32_t pos = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    if (value == 0) {
        append_char(buf, &pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        append_char(buf, &pos, cap, tmp[--n]);
    }
}

void taskmgr_format_hex_fixed(char *buf, uint32_t cap, uint64_t value, uint32_t digits)
{
    const char *hex = "0123456789abcdef";
    uint32_t pos = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    append_text(buf, &pos, cap, "0x");
    for (int32_t shift = (int32_t)(digits * 4); shift > 0; shift -= 4) {
        append_char(buf, &pos, cap, hex[(value >> (uint32_t)(shift - 4)) & 0xf]);
    }
}

void taskmgr_format_percent(char *buf, uint32_t cap, uint32_t value)
{
    char tail[24];
    uint32_t pos = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    taskmgr_format_dec(tail, sizeof(tail), value);
    append_text(buf, &pos, cap, tail);
    append_text(buf, &pos, cap, "%");
}

void taskmgr_format_kib(char *buf, uint32_t cap, uint64_t kib)
{
    char tail[24];
    uint32_t pos = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    taskmgr_format_dec(tail, sizeof(tail), kib);
    append_text(buf, &pos, cap, tail);
    append_text(buf, &pos, cap, " KiB");
}

void taskmgr_format_memory(char *buf, uint32_t cap, uint32_t kib)
{
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    uint64_t bytes = (uint64_t)kib * 1024ULL;
    uint64_t unit_size = 1ULL;
    uint64_t whole;
    uint64_t fraction;
    uint32_t unit = 0;
    uint32_t pos = 0;
    char tail[24];

    if (!buf || !cap) return;
    while (unit + 1U < sizeof(units) / sizeof(units[0]) &&
           bytes >= unit_size * 1024ULL) {
        unit_size *= 1024ULL;
        ++unit;
    }
    whole = bytes / unit_size;
    /* Keep one useful decimal place for small non-integral values without
     * making the process list jump in width on every refresh. */
    fraction = ((bytes % unit_size) * 10ULL + unit_size / 2ULL) / unit_size;
    if (fraction == 10ULL) {
        ++whole;
        fraction = 0;
    }

    buf[0] = 0;
    taskmgr_format_dec(tail, sizeof(tail), whole);
    append_text(buf, &pos, cap, tail);
    if (unit != 0 && whole < 10ULL && fraction != 0) {
        char digit[2] = {(char)('0' + fraction), 0};
        append_char(buf, &pos, cap, '.');
        append_text(buf, &pos, cap, digit);
    }
    append_char(buf, &pos, cap, ' ');
    append_text(buf, &pos, cap, units[unit]);
}

void taskmgr_format_uptime(char *buf, uint32_t cap, uint64_t ms)
{
    uint64_t seconds = ms / 1000ULL;
    uint64_t hours = seconds / 3600ULL;
    uint64_t minutes = (seconds / 60ULL) % 60ULL;
    uint64_t secs = seconds % 60ULL;
    uint32_t pos = 0;
    if (!buf || !cap) return;
    buf[0] = 0;
    taskmgr_format_dec(buf, cap, hours);
    while (buf[pos]) ++pos;
    append_text(buf, &pos, cap, ":");
    if (minutes < 10) {
        append_text(buf, &pos, cap, "0");
    }
    {
        char tail[8];
        taskmgr_format_dec(tail, sizeof(tail), minutes);
        append_text(buf, &pos, cap, tail);
    }
    append_text(buf, &pos, cap, ":");
    if (secs < 10) {
        append_text(buf, &pos, cap, "0");
    }
    {
        char tail[8];
        taskmgr_format_dec(tail, sizeof(tail), secs);
        append_text(buf, &pos, cap, tail);
    }
}

const char *taskmgr_state_name(uint32_t state)
{
    switch (state) {
    case 0:
        return "ready";
    case 1:
        return "run";
    case 2:
        return "sleep";
    case 3:
        return "exit";
    default:
        return "?";
    }
}

const char *taskmgr_kind_name(uint32_t kind)
{
    return kind == 1 ? "user" : "kern";
}

uint32_t taskmgr_task_cpu_percent(uint64_t used_ticks, uint64_t tick_delta,
                                  uint32_t fallback_percent, int have_fallback)
{
    uint32_t percent;
    if (tick_delta) {
        percent = (uint32_t)((used_ticks * 100ULL) / tick_delta);
    } else if (have_fallback) {
        percent = fallback_percent;
    } else {
        percent = 0;
    }
    return percent > 100U ? 100U : percent;
}

uint32_t taskmgr_busy_percent(uint64_t previous_busy, uint64_t previous_idle,
                              uint64_t busy, uint64_t idle,
                              int have_previous, uint32_t fallback_percent)
{
    uint64_t old_total = previous_busy + previous_idle;
    uint64_t new_total = busy + idle;
    uint32_t percent;
    if (old_total && new_total > old_total) {
        uint64_t delta_total = new_total - old_total;
        uint64_t delta_busy = busy >= previous_busy ? busy - previous_busy : 0;
        percent = delta_total ? (uint32_t)((delta_busy * 100ULL) / delta_total) : 0;
    } else if (!have_previous) {
        percent = new_total ? (uint32_t)((busy * 100ULL) / new_total) : 0;
    } else {
        percent = fallback_percent;
    }
    return percent > 100U ? 100U : percent;
}

uint32_t taskmgr_mem_percent(uint64_t total_kib, uint64_t free_kib)
{
    uint32_t percent = 0;
    if (total_kib && total_kib >= free_kib) {
        uint64_t used = total_kib - free_kib;
        percent = (uint32_t)((used * 100ULL) / total_kib);
    }
    return percent > 100U ? 100U : percent;
}

int taskmgr_task_killable(uint32_t pid, uint32_t self_pid, uint32_t kind,
                          uint32_t state, uint32_t flags)
{
    if (pid == 0 || kind != 1 || state == 3 || (flags & 1u) || pid == self_pid) {
        return 0;
    }
    return 1;
}

void taskmgr_perf_history_init(struct taskmgr_perf_history *history)
{
    uint32_t i;
    uint32_t core;
    if (!history) return;
    for (i = 0; i < TASKMGR_PERF_HISTORY; ++i) {
        history->memory[i] = 0;
        history->gpu[i] = TASKMGR_PERF_MISSING;
    }
    for (core = 0; core < TASKMGR_MAX_CORES; ++core) {
        for (i = 0; i < TASKMGR_PERF_HISTORY; ++i) {
            history->core[core][i] = 0;
        }
    }
    history->head = 0;
    history->count = 0;
}

uint32_t taskmgr_perf_history_push(struct taskmgr_perf_history *history,
                                   uint32_t memory_percent,
                                   uint32_t gpu_percent, int gpu_valid)
{
    uint32_t slot;
    if (!history) return 0;
    slot = history->head;
    history->memory[slot] = (uint8_t)(memory_percent > 100U ? 100U : memory_percent);
    history->gpu[slot] = gpu_valid
                             ? (uint8_t)(gpu_percent > 100U ? 100U : gpu_percent)
                             : TASKMGR_PERF_MISSING;
    history->head = (slot + 1U) % TASKMGR_PERF_HISTORY;
    if (history->count < TASKMGR_PERF_HISTORY) {
        ++history->count;
    }
    return slot;
}

void taskmgr_perf_history_clear_gpu(struct taskmgr_perf_history *history)
{
    uint32_t i;
    if (!history) return;
    for (i = 0; i < TASKMGR_PERF_HISTORY; ++i) {
        history->gpu[i] = TASKMGR_PERF_MISSING;
    }
}

uint32_t taskmgr_tree_order(const uint32_t *pids, const uint32_t *parents,
                            uint32_t count, struct taskmgr_tree_row *rows,
                            uint32_t capacity)
{
    /* Depth-first parent-before-children order; a node is a root when its
     * parent is itself or absent from the list.  Cycles have no root, so
     * unvisited nodes are appended flat after the rooted walk instead of
     * being dropped. */
    uint8_t visited[count ? count : 1];
    uint32_t written = 0;
    uint32_t i;
    uint32_t j;
    if (!pids || !parents || !rows || !capacity) return 0;

    for (i = 0; i < count; ++i) visited[i] = 0;

    for (i = 0; i < count && written < capacity; ++i) {
        int is_root = 1;
        if (visited[i]) continue;
        if (parents[i] != pids[i]) {
            for (j = 0; j < count; ++j) {
                if (j != i && pids[j] == parents[i]) {
                    is_root = 0;
                    break;
                }
            }
        }
        if (!is_root) continue;
        {
            uint32_t stack[count ? count : 1];
            uint32_t depth[count ? count : 1];
            uint32_t size = 1;
            stack[0] = i;
            depth[0] = 0;
            visited[i] = 1;
            while (size && written < capacity) {
                uint32_t node;
                uint32_t node_depth;
                --size;
                node = stack[size];
                node_depth = depth[size];
                rows[written].index = node;
                rows[written].depth = node_depth;
                ++written;
                /* Push children in reverse input order so they pop in order. */
                for (j = count; j-- > 0;) {
                    if (!visited[j] && j != node && parents[j] == pids[node]) {
                        visited[j] = 1;
                        stack[size] = j;
                        depth[size] = node_depth + 1U;
                        ++size;
                    }
                }
            }
        }
    }
    for (i = 0; i < count && written < capacity; ++i) {
        if (!visited[i]) {
            rows[written].index = i;
            rows[written].depth = 0;
            ++written;
        }
    }
    return written;
}
