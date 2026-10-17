extern "C" {
#include "model.h"
#include "gpu_sample.h"
#include <reliefos/auth.h>
#include <reliefos/gui.h>
#include <reliefos/launch.h>
#include <reliefos/startup.h>
#include <reliefos/system.h>
}

#include "fltk_support.h"

#include <FL/Fl_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

enum {
    TASKMGR_TAB_PROCESSES = 0,
    TASKMGR_TAB_PERFORMANCE = 1,
    TASKMGR_TAB_STARTUP = 2,
};

enum {
    TASKMGR_ACTION_REFRESH = 1,
    TASKMGR_ACTION_END = 2,
    TASKMGR_ACTION_DETAILS = 3,
    TASKMGR_ACTION_ABOUT = 4,
    TASKMGR_ACTION_PROCESSES = 5,
    TASKMGR_ACTION_PERFORMANCE = 6,
    TASKMGR_ACTION_STARTUP = 7,
    TASKMGR_ACTION_ENABLE = 8,
    TASKMGR_ACTION_REMOVE = 9,
    TASKMGR_ACTION_SERVICE_MANAGER = 10,
};

Fl_Double_Window *shell;
Fl_Menu_Bar *menu_bar;
Fl_Tabs *tab_stack;
Fl_Group *page_processes;
Fl_Group *page_performance;
Fl_Group *page_startup;
Fl_Browser *process_list;
Fl_Browser *startup_list_widget;
Fl_Choice *startup_choice;
Fl_Output *startup_username;
Fl_Output *status_output;
Fl_Button *button_end_task;
Fl_Button *button_startup_toggle;
Fl_Button *button_startup_remove;
Fl_Menu_Button *process_popup;
Fl_Widget *performance_widget;

struct reliefos_task_info tasks[RELIEFOS_TASK_MAX];
struct reliefos_task_info previous_tasks[RELIEFOS_TASK_MAX];
uint32_t task_cpu_percent[RELIEFOS_TASK_MAX];
uint32_t previous_task_cpu_percent[RELIEFOS_TASK_MAX];
uint32_t task_count, previous_task_count;
uint64_t previous_sample_total;
struct taskmgr_tree_row tree_rows[RELIEFOS_TASK_MAX];
uint32_t tree_count;
uint32_t selected_pid;

struct taskmgr_perf_history history;
struct reliefos_perf_info perf_info;
uint64_t last_busy_ticks, last_idle_ticks;
uint64_t last_cpu_busy[RELIEFOS_PERF_MAX_CPUS];
uint64_t last_cpu_idle[RELIEFOS_PERF_MAX_CPUS];
uint32_t cpu_percent_by_core[RELIEFOS_PERF_MAX_CPUS];
uint32_t cpu_percent, mem_percent;
int cpu_snapshot_valid, perf_valid;
struct taskmgr_gpu_sample gpu_sample;

struct reliefos_startup_entry startup_entries[RELIEFOS_STARTUP_MAX_ENTRIES];
uint32_t startup_entry_count;
struct reliefos_user_info startup_users[16];
uint32_t startup_user_count, startup_selected_uid;
uint32_t selected_startup_id;
int startup_available;
int selected_startup = -1;
int active_tab = TASKMGR_TAB_PROCESSES;
int updating;

static const uint32_t core_palette[] = {
    0x00B81C1C, 0x001CB89E, 0x00000080, 0x007AB800,
    0x009E1CB8, 0x0000B81F, 0x00B8A753, 0x003737B8,
    0x00B87A00, 0x005C8039, 0x00006699, 0x006D1380,
    0x00804000, 0x00B8005C, 0x005396B8, 0x00B85396,
    0x00158000, 0x0074B853, 0x00008040, 0x00800000,
    0x00800040, 0x00B8B800, 0x005C00B8, 0x003DB800,
    0x0000B85C, 0x008553B8, 0x00B86237, 0x001F00B8,
    0x00398068, 0x00263580, 0x006A8000, 0x00803939,
};

struct PerfPlotRect { int x, y, w, h; };

void refresh_all();
void update_controls();
void select_page(int tab);
void perform_action(uint32_t action);

void set_status(const char *text)
{
    if (status_output) status_output->value(text ? text : "");
}

int task_index_by_pid(uint32_t pid)
{
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i].pid == pid) return (int)i;
    return -1;
}

int previous_index_by_pid(uint32_t pid)
{
    for (uint32_t i = 0; i < previous_task_count; ++i)
        if (previous_tasks[i].pid == pid) return (int)i;
    return -1;
}

struct reliefos_task_info *selected_task()
{
    int index = selected_pid ? task_index_by_pid(selected_pid) : -1;
    return index >= 0 ? &tasks[index] : NULL;
}

const char *task_user_name(const struct reliefos_task_info *task)
{
    if (task && task->username[0]) return task->username;
    return task && task->uid ? T("Unknown") : T("System");
}

const char *task_privilege_name(const struct reliefos_task_info *task)
{
    if (!task || !task->uid) return T("System");
    if (task->flags & RELIEFOS_TASK_SNAPSHOT_FLAG_ELEVATED_ADMIN) return T("Elevated");
    if (task->role == RELIEFOS_AUTH_ROLE_ADMIN) return T("Admin");
    return T("Standard");
}

int selected_task_killable()
{
    struct reliefos_task_info *task = selected_task();
    if (!task) return 0;
    return taskmgr_task_killable(task->pid, (uint32_t)getpid(), task->kind,
                                 task->state, task->flags);
}

void startup_command_line(char *text, uint32_t cap,
                          const struct reliefos_startup_command *command)
{
    uint32_t used = 0;
    text[0] = 0;
    for (uint32_t i = 0; command && command->path[i] && used + 1 < cap; ++i)
        text[used++] = command->path[i];
    for (uint32_t a = 0; command && a < command->argc; ++a) {
        if (used + 1 < cap) text[used++] = ' ';
        for (uint32_t i = 0; command->args[a][i] && used + 1 < cap; ++i)
            text[used++] = command->args[a][i];
    }
    text[used] = 0;
}

void update_controls()
{
    struct reliefos_task_info *task = selected_task();
    struct reliefos_startup_entry *entry =
        (active_tab == TASKMGR_TAB_STARTUP && selected_startup >= 0 &&
         (uint32_t)selected_startup < startup_entry_count)
            ? &startup_entries[selected_startup] : NULL;
    if (button_end_task) {
        if (selected_task_killable()) button_end_task->activate();
        else button_end_task->deactivate();
    }
    if (button_startup_toggle) {
        button_startup_toggle->copy_label(entry && entry->enabled ? T("Disable") : T("Enable"));
        if (entry && startup_available) button_startup_toggle->activate();
        else button_startup_toggle->deactivate();
    }
    if (button_startup_remove) {
        if (entry && startup_available) button_startup_remove->activate();
        else button_startup_remove->deactivate();
    }
    (void)task;
}

void refresh_tasks()
{
    char lines[RELIEFOS_TASK_MAX][RELIEFOS_TASK_NAME_LEN + 300];
    const char *rows[RELIEFOS_TASK_MAX];
    uint32_t pids[RELIEFOS_TASK_MAX];
    uint32_t parents[RELIEFOS_TASK_MAX];
    uint64_t next_tick = 0;
    uint64_t sample_total = perf_info.busy_ticks + perf_info.idle_ticks;
    uint64_t tick_delta = 0;
    int select_position = 0;
    int count = reliefos_task_snapshot(tasks, RELIEFOS_TASK_MAX, &next_tick);
    (void)next_tick;
    task_count = count > 0 ? (uint32_t)count : 0;
    if (task_count > RELIEFOS_TASK_MAX) task_count = RELIEFOS_TASK_MAX;
    if (previous_sample_total && sample_total > previous_sample_total)
        tick_delta = sample_total - previous_sample_total;
    for (uint32_t i = 0; i < task_count; ++i) {
        int previous = previous_index_by_pid(tasks[i].pid);
        uint64_t used_ticks = 0;
        uint32_t fallback = 0;
        int have_fallback = 0;
        if (previous >= 0 && (uint32_t)previous < previous_task_count &&
            tasks[i].cpu_ticks >= previous_tasks[previous].cpu_ticks) {
            used_ticks = tasks[i].cpu_ticks - previous_tasks[previous].cpu_ticks;
            fallback = previous_task_cpu_percent[previous];
            have_fallback = 1;
        }
        task_cpu_percent[i] = taskmgr_task_cpu_percent(used_ticks, tick_delta,
                                                       fallback, have_fallback);
        pids[i] = tasks[i].pid;
        parents[i] = tasks[i].parent_pid;
    }
    tree_count = taskmgr_tree_order(pids, parents, task_count, tree_rows,
                                    RELIEFOS_TASK_MAX);
    if (process_list) {
        int old_top = process_list->topline();
        updating = 1;
        process_list->clear();
        process_list->format_char(0);
        for (uint32_t i = 0; i < tree_count; ++i) {
            uint32_t index = tree_rows[i].index;
            char pid[16], cpu[16], mem[24];
            taskmgr_format_dec(pid, sizeof(pid), tasks[index].pid);
            taskmgr_format_percent(cpu, sizeof(cpu), task_cpu_percent[index]);
            taskmgr_format_memory(mem, sizeof(mem), tasks[index].memory_kib);
            unsigned indent = tree_rows[i].depth * 2U;
            if (indent > 120U) indent = 120U;
            snprintf(lines[i], sizeof(lines[i]), "%*s%s  PID %s  CPU %s  MEM %s  %s  %s  %s",
                     (int)indent, "", tasks[index].name, pid, cpu, mem,
                     taskmgr_state_name(tasks[index].state), task_user_name(&tasks[index]),
                     task_privilege_name(&tasks[index]));
            rows[i] = lines[i];
            process_list->add(rows[i]);
            if (selected_pid && tasks[index].pid == selected_pid)
                select_position = (int)i + 1;
        }
        if (select_position) process_list->value(select_position);
        else if (selected_pid) selected_pid = 0;
        if (old_top > 0 && (uint32_t)old_top <= tree_count) process_list->topline(old_top);
        updating = 0;
    }
    previous_task_count = task_count;
    previous_sample_total = sample_total;
    for (uint32_t i = 0; i < task_count; ++i) {
        previous_task_cpu_percent[i] = task_cpu_percent[i];
        previous_tasks[i] = tasks[i];
    }
}

void refresh_startup_users()
{
    struct reliefos_user_info current = {};
    struct reliefos_user_info *allocated = NULL;
    uint32_t count = 0;
    startup_user_count = 0;
    if (reliefos_auth_current(&current) < 0) {
        startup_available = 0;
        return;
    }
    if (current.role == RELIEFOS_AUTH_ROLE_ADMIN &&
        reliefos_auth_users_alloc(&allocated, 0, &count) == 0 && count &&
        count <= sizeof(startup_users) / sizeof(startup_users[0])) {
        for (uint32_t i = 0; i < count; ++i) startup_users[i] = allocated[i];
        startup_user_count = count;
        free(allocated);
    } else {
        free(allocated);
        startup_users[0] = current;
        startup_user_count = 1;
    }
    for (uint32_t i = 0; i < startup_user_count; ++i) {
        if (startup_users[i].uid == startup_selected_uid) return;
    }
    startup_selected_uid = startup_user_count ? startup_users[0].uid : 0;
}

void refresh_startup_choice()
{
    static uint32_t built_count;
    static uint32_t built_uids[16];
    int changed = built_count != startup_user_count;
    for (uint32_t i = 0; !changed && i < startup_user_count; ++i)
        if (built_uids[i] != startup_users[i].uid) changed = 1;
    if (changed && startup_choice) {
        updating = 1;
        startup_choice->clear();
        for (uint32_t i = 0; i < startup_user_count; ++i) {
            char item[32];
            snprintf(item, sizeof(item), "UID %u", startup_users[i].uid);
            startup_choice->add(item);
            built_uids[i] = startup_users[i].uid;
        }
        built_count = startup_user_count;
        updating = 0;
    }
    if (startup_choice) {
        int selected = 0;
        for (uint32_t i = 0; i < startup_user_count; ++i)
            if (startup_users[i].uid == startup_selected_uid) selected = (int)i;
        updating = 1;
        startup_choice->value(selected);
        updating = 0;
    }
    if (startup_username) {
        const char *name = T("Unavailable");
        for (uint32_t i = 0; i < startup_user_count; ++i)
            if (startup_users[i].uid == startup_selected_uid) name = startup_users[i].username;
        startup_username->value(name);
    }
}

void refresh_startup_entries()
{
    char lines[RELIEFOS_STARTUP_MAX_ENTRIES][800];
    uint32_t count = 0;
    int old_top = startup_list_widget ? startup_list_widget->topline() : 1;
    if (!startup_user_count ||
        reliefos_startup_list(startup_selected_uid, startup_entries,
                              RELIEFOS_STARTUP_MAX_ENTRIES, &count) < 0) {
        startup_available = 0;
        startup_entry_count = 0;
        count = 0;
    } else {
        startup_available = 1;
        startup_entry_count = count > RELIEFOS_STARTUP_MAX_ENTRIES
                                  ? RELIEFOS_STARTUP_MAX_ENTRIES : count;
        count = startup_entry_count;
    }
    selected_startup = -1;
    for (uint32_t i = 0; i < count; ++i)
        if (selected_startup_id && startup_entries[i].id == selected_startup_id)
            selected_startup = (int)i;
    if (startup_list_widget) {
        updating = 1;
        startup_list_widget->clear();
        startup_list_widget->format_char(0);
        for (uint32_t i = 0; i < count; ++i) {
            char command[736];
            startup_command_line(command, sizeof(command), &startup_entries[i].command);
            snprintf(lines[i], sizeof(lines[i]), "%s  %s",
                     startup_entries[i].enabled ? T("Enabled") : T("Disabled"), command);
            startup_list_widget->add(lines[i]);
        }
        if (selected_startup >= 0) startup_list_widget->value(selected_startup + 1);
        else selected_startup_id = 0;
        if (old_top > 0 && (uint32_t)old_top <= count) startup_list_widget->topline(old_top);
        updating = 0;
    }
}

void refresh_startup()
{
    refresh_startup_users();
    refresh_startup_choice();
    refresh_startup_entries();
    update_controls();
}

void refresh_performance()
{
    struct reliefos_perf_info next = {};
    gpu_sdk_info_t next_gpu = {};
    next_gpu.size = sizeof(gpu_sdk_info_t);
    next_gpu.version = GPU_SDK_ABI_VERSION;
    int gpu_result = gpu_sdk_info(&next_gpu);
    if (taskmgr_gpu_sample_update(&gpu_sample, gpu_result < 0 ? NULL : &next_gpu))
        taskmgr_perf_history_clear_gpu(&history);
    if (reliefos_perf_info(&next) < 0) {
        perf_valid = 0;
        set_status(T("Performance data unavailable"));
        return;
    }
    cpu_percent = taskmgr_busy_percent(last_busy_ticks, last_idle_ticks,
                                       next.busy_ticks, next.idle_ticks,
                                       cpu_snapshot_valid, cpu_percent);
    mem_percent = taskmgr_mem_percent(next.total_memory_kib, next.free_memory_kib);
    uint32_t slot = taskmgr_perf_history_push(&history, mem_percent,
                                              gpu_sample.percent, gpu_sample.valid);
    uint32_t cpu_count = next.cpu_count;
    if (cpu_count > RELIEFOS_PERF_MAX_CPUS) cpu_count = RELIEFOS_PERF_MAX_CPUS;
    for (uint32_t i = 0; i < RELIEFOS_PERF_MAX_CPUS; ++i) {
        if (i >= cpu_count || !next.cpus[i].online) {
            cpu_percent_by_core[i] = 0;
            history.core[i][slot] = TASKMGR_PERF_MISSING;
        } else {
            cpu_percent_by_core[i] = taskmgr_busy_percent(
                last_cpu_busy[i], last_cpu_idle[i], next.cpus[i].busy_ticks,
                next.cpus[i].idle_ticks, cpu_snapshot_valid, cpu_percent_by_core[i]);
            history.core[i][slot] = (uint8_t)cpu_percent_by_core[i];
        }
        last_cpu_busy[i] = next.cpus[i].busy_ticks;
        last_cpu_idle[i] = next.cpus[i].idle_ticks;
    }
    perf_info = next;
    last_busy_ticks = next.busy_ticks;
    last_idle_ticks = next.idle_ticks;
    cpu_snapshot_valid = 1;
    perf_valid = 1;
}

Fl_Color core_color(uint32_t core)
{
    uint32_t rgb = core_palette[core % (sizeof(core_palette) / sizeof(core_palette[0]))];
    return fl_rgb_color((uchar)((rgb >> 16) & 255U),
                        (uchar)((rgb >> 8) & 255U), (uchar)(rgb & 255U));
}

void draw_history_line(const PerfPlotRect *plot, const uint8_t *series, Fl_Color color)
{
    if (!history.count) return;
    int previous_set = 0, previous_x = 0, previous_y = 0;
    for (uint32_t i = 0; i < history.count; ++i) {
        uint32_t index = (history.head + TASKMGR_PERF_HISTORY - history.count + i) %
                         TASKMGR_PERF_HISTORY;
        uint32_t slot = TASKMGR_PERF_HISTORY - history.count + i;
        if (series[index] == TASKMGR_PERF_MISSING) {
            previous_set = 0;
            continue;
        }
        uint32_t value = series[index] > 100U ? 100U : series[index];
        int px = plot->x + 1 + (int)((slot * (uint32_t)(plot->w - 3)) /
                                      (TASKMGR_PERF_HISTORY - 1U));
        int py = plot->y + plot->h - 2 -
                 (int)((value * (uint32_t)(plot->h - 3)) / 100U);
        if (previous_set) {
            fl_color(color);
            fl_line(previous_x, previous_y, px, py);
        }
        previous_x = px;
        previous_y = py;
        previous_set = 1;
    }
}

void draw_graph_frame(int x, int y, int w, int h, const char *title,
                      const char *current, int available, PerfPlotRect *plot)
{
    fl_color(available ? FL_DARK3 : FL_INACTIVE_COLOR);
    fl_draw(title, x + 8, y + 18);
    if (current) fl_draw(current, x + w - 64, y + 18);
    plot->x = x + 8;
    plot->y = y + 26;
    plot->w = w > 16 ? w - 16 : 1;
    plot->h = h > 34 ? h - 34 : 1;
    fl_color(FL_WHITE);
    fl_rectf(plot->x, plot->y, plot->w, plot->h);
    fl_color(FL_DARK2);
    fl_rect(plot->x, plot->y, plot->w, plot->h);
    for (int step = 1; step < 4; ++step) {
        int gy = plot->y + ((plot->h - 1) * step) / 4;
        fl_line(plot->x + 1, gy, plot->x + plot->w - 2, gy);
    }
    if (plot->h >= 40) {
        fl_color(FL_DARK2);
        fl_draw("100%", plot->x + 3, plot->y + 14);
        fl_draw("0%", plot->x + 3, plot->y + plot->h - 4);
    }
}

void draw_graph(int x, int y, int w, int h, const char *title,
                const char *current, const uint8_t *series, Fl_Color color)
{
    PerfPlotRect plot;
    if (w < 96 || h < 32) return;
    draw_graph_frame(x, y, w, h, title, current, series != NULL, &plot);
    if (series && plot.w >= 4 && plot.h >= 4) draw_history_line(&plot, series, color);
}

void draw_performance(int x, int y, int width, int height)
{
    char value[64], current[32], core_label[32];
    int content_w = width > 24 ? width - 24 : 1;
    int content_bottom = y + height - 8;
    if (!perf_valid) {
        fl_color(FL_FOREGROUND_COLOR);
        fl_draw(T("Performance data unavailable"), x + 24, y + 32);
        return;
    }
    int columns = content_w >= 700 ? 3 : 1;
    int gap = 8;
    int graph_w = (content_w - gap * (columns - 1)) / columns;
    int graph_h = columns == 1 ? 92 : (height > 440 ? 126 : 100);
    int top = y + 8;
    for (uint32_t i = 0; i < 3; ++i) {
        int gx = x + 8 + (int)(i % (uint32_t)columns) * (graph_w + gap);
        int gy = top + (int)(i / (uint32_t)columns) * (graph_h + gap);
        if (i == 0) {
            taskmgr_format_percent(current, sizeof(current), cpu_percent);
            PerfPlotRect plot;
            draw_graph_frame(gx, gy, graph_w, graph_h, T("CPU Usage"), current, 1, &plot);
            uint32_t cores = perf_info.cpu_count > RELIEFOS_PERF_MAX_CPUS
                                 ? RELIEFOS_PERF_MAX_CPUS : perf_info.cpu_count;
            for (uint32_t core = 0; core < cores; ++core)
                draw_history_line(&plot, history.core[core], core_color(core));
        } else if (i == 1) {
            taskmgr_format_percent(current, sizeof(current), mem_percent);
            draw_graph(gx, gy, graph_w, graph_h, T("Memory Usage"), current,
                       history.memory, FL_SELECTION_COLOR);
        } else {
            if (gpu_sample.valid)
                taskmgr_format_percent(current, sizeof(current), gpu_sample.percent);
            else snprintf(current, sizeof(current), "%s", "N/A");
            draw_graph(gx, gy, graph_w, graph_h,
                       graph_w < 210 ? T("GPU (est.)") : T("GPU (estimated)"),
                       current, gpu_sample.available ? history.gpu : NULL,
                       fl_rgb_color(59, 98, 166));
        }
    }
    uint32_t cpu_count = perf_info.cpu_count > RELIEFOS_PERF_MAX_CPUS
                             ? RELIEFOS_PERF_MAX_CPUS : perf_info.cpu_count;
    int info_y = top + graph_h + gap + 20;
    fl_color(FL_FOREGROUND_COLOR);
    fl_draw(T("Per-core usage"), x + 24, info_y);
    info_y += 26;
    int core_columns = cpu_count > 16U ? 3 : (cpu_count > 8U ? 2 : 1);
    if (content_w < 200) core_columns = 1;
    int column_w = content_w / core_columns;
    for (uint32_t i = 0; i < cpu_count; ++i) {
        int cx = x + 24 + ((int)i % core_columns) * column_w;
        int cy = info_y + ((int)i / core_columns) * 27;
        if (cy + 18 > content_bottom) break;
        snprintf(core_label, sizeof(core_label), "CPU %u", i);
        fl_color(core_color(i));
        fl_rectf(cx, cy + 3, 7, 8);
        fl_color(FL_FOREGROUND_COLOR);
        fl_draw(core_label, cx + 11, cy + 12);
        int progress_x = cx + 62;
        int progress_w = column_w > 112 ? column_w - 112 : 28;
        fl_color(FL_DARK2);
        fl_rect(progress_x, cy, progress_w, 16);
        fl_color(core_color(i));
        fl_rectf(progress_x + 1, cy + 1,
                 (progress_w - 2) * (int)cpu_percent_by_core[i] / 100, 14);
        taskmgr_format_percent(value, sizeof(value), cpu_percent_by_core[i]);
        fl_color(FL_FOREGROUND_COLOR);
        fl_draw(value, cx + column_w - 42, cy + 12);
    }
    info_y += (int)((cpu_count + (uint32_t)core_columns - 1U) /
                    (uint32_t)core_columns) * 27 + 5;
    uint64_t used_kib = perf_info.total_memory_kib >= perf_info.free_memory_kib
                            ? perf_info.total_memory_kib - perf_info.free_memory_kib : 0;
    taskmgr_format_kib(value, sizeof(value), perf_info.total_memory_kib);
    fl_draw(T("Total memory:"), x + 24, info_y + 12);
    fl_draw(value, x + 174, info_y + 12);
    info_y += 19;
    taskmgr_format_kib(value, sizeof(value), used_kib);
    fl_draw(T("Used memory:"), x + 24, info_y + 12);
    fl_draw(value, x + 174, info_y + 12);
    info_y += 19;
    taskmgr_format_kib(value, sizeof(value), perf_info.free_memory_kib);
    fl_draw(T("Free memory:"), x + 24, info_y + 12);
    fl_draw(value, x + 174, info_y + 12);
    info_y += 19;
    taskmgr_format_uptime(value, sizeof(value), perf_info.uptime_ms);
    fl_draw(T("Uptime:"), x + 24, info_y + 12);
    fl_draw(value, x + 174, info_y + 12);
    info_y += 24;
    if (info_y <= content_bottom) {
        char label[96], values[96];
        snprintf(label, sizeof(label), "%s%u / %s%u", T("Tasks "),
                 perf_info.task_count, T("Running "), perf_info.running_tasks);
        snprintf(values, sizeof(values), "%s%u / %s%u", T("Ready "),
                 perf_info.ready_tasks, T("Sleeping "), perf_info.sleeping_tasks);
        fl_draw(label, x + 24, info_y + 12);
        fl_draw(values, x + 174, info_y + 12);
    }
}

class PerformanceView : public Fl_Widget {
public:
    PerformanceView(int x, int y, int w, int h, const char *label = NULL)
        : Fl_Widget(x, y, w, h, label) {}
    void draw() override
    {
        fl_push_clip(x(), y(), w(), h());
        fl_color(FL_BACKGROUND_COLOR);
        fl_rectf(x(), y(), w(), h());
        draw_performance(x(), y(), w(), h());
        fl_pop_clip();
    }
};

int confirm_dialog(const char *title, const char *message)
{
    fl_message_title(title);
    return fl_choice("%s", T("No"), T("Yes"), NULL, message ? message : "") == 1;
}

void kill_selected_task()
{
    struct reliefos_task_info *task = selected_task();
    if (!task) {
        set_status(T("No task selected"));
        return;
    }
    if (!selected_task_killable()) {
        set_status(T("Cannot end protected or non-user task"));
        return;
    }
    char prompt[RELIEFOS_TASK_NAME_LEN + 96];
    snprintf(prompt, sizeof(prompt), T("End task %s (PID %u)?"), task->name, task->pid);
    if (!confirm_dialog(T("End Task"), prompt)) return;
    if (reliefos_task_kill(task->pid) < 0) {
        set_status(T("End Task failed"));
        return;
    }
    set_status(T("Task ended"));
    refresh_all();
}

void show_task_details()
{
    struct reliefos_task_info *task = selected_task();
    if (!task) {
        set_status(T("No task selected"));
        return;
    }
    struct reliefos_task_info snapshot = *task;
    char body[RELIEFOS_TASK_NAME_LEN + 768];
    char pid[16], ppid[16], cr3[24], entry[24], wake[16], ticks[32], tick_count_text[24], mem[24];
    taskmgr_format_dec(pid, sizeof(pid), snapshot.pid);
    taskmgr_format_dec(ppid, sizeof(ppid), snapshot.parent_pid);
    taskmgr_format_hex_fixed(cr3, sizeof(cr3), snapshot.cr3, 12);
    taskmgr_format_hex_fixed(entry, sizeof(entry), snapshot.entry, 12);
    taskmgr_format_dec(wake, sizeof(wake), snapshot.wake_tick);
    taskmgr_format_dec(tick_count_text, sizeof(tick_count_text), snapshot.cpu_ticks);
    snprintf(ticks, sizeof(ticks), "%s %s", tick_count_text, T("ticks"));
    taskmgr_format_memory(mem, sizeof(mem), snapshot.memory_kib);
    snprintf(body, sizeof(body),
             "%s %s\nPID: %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n"
             "CR3: %s\nEntry: %s",
             T("Name:"), snapshot.name,
             pid,
             T("Parent PID:"), ppid,
             T("State:"), taskmgr_state_name(snapshot.state),
             T("Kind:"), taskmgr_kind_name(snapshot.kind),
             T("User:"), task_user_name(&snapshot),
             T("Privileges:"), task_privilege_name(&snapshot),
             T("CPU time:"), ticks,
             T("Memory:"), mem,
             T("Wake tick:"), wake,
             cr3, entry);
    fl_message_title(T("Task Details"));
    fl_message("%s", body);
}

void toggle_selected_startup_entry()
{
    struct reliefos_startup_entry *entry =
        selected_startup >= 0 && (uint32_t)selected_startup < startup_entry_count
            ? &startup_entries[selected_startup] : NULL;
    if (!entry) {
        set_status(T("No startup app selected"));
        return;
    }
    if (reliefos_startup_set_enabled(startup_selected_uid, entry->id,
                                     !entry->enabled) < 0) {
        set_status(T("Could not change startup app"));
        return;
    }
    set_status(entry->enabled ? T("Startup app disabled") : T("Startup app enabled"));
    refresh_startup_entries();
    update_controls();
}

void remove_selected_startup_entry()
{
    struct reliefos_startup_entry *entry =
        selected_startup >= 0 && (uint32_t)selected_startup < startup_entry_count
            ? &startup_entries[selected_startup] : NULL;
    if (!entry) {
        set_status(T("No startup app selected"));
        return;
    }
    if (!confirm_dialog(T("Remove Startup App"), T("Remove the selected startup app?"))) return;
    if (reliefos_startup_remove(startup_selected_uid, entry->id) < 0) {
        set_status(T("Could not remove startup app"));
        return;
    }
    selected_startup_id = 0;
    set_status(T("Startup app removed"));
    refresh_startup_entries();
    update_controls();
}

void open_service_manager()
{
    const char *program = reliefos_launch_builtin_path("servicemgr");
    if (!program || !program[0]) {
        set_status(T("Service Manager is unavailable"));
        return;
    }
    char *args[] = {(char *)program, NULL};
    if (reliefos_launch_argv(args) < 0) set_status(T("Could not open Service Manager"));
    else set_status(T("Service Manager launched"));
}

void show_about()
{
    fl_message_title(T("Task Manager"));
    fl_message("%s", T("Live task and performance snapshot from the system."));
}

void select_page(int tab)
{
    active_tab = tab;
    if (tab == TASKMGR_TAB_PROCESSES) tab_stack->value(page_processes);
    else if (tab == TASKMGR_TAB_PERFORMANCE) tab_stack->value(page_performance);
    else {
        tab_stack->value(page_startup);
        refresh_startup();
    }
    update_controls();
}

void perform_action(uint32_t action)
{
    switch (action) {
    case TASKMGR_ACTION_REFRESH: refresh_all(); break;
    case TASKMGR_ACTION_END: kill_selected_task(); break;
    case TASKMGR_ACTION_DETAILS: show_task_details(); break;
    case TASKMGR_ACTION_ABOUT: show_about(); break;
    case TASKMGR_ACTION_PROCESSES: select_page(TASKMGR_TAB_PROCESSES); break;
    case TASKMGR_ACTION_PERFORMANCE: select_page(TASKMGR_TAB_PERFORMANCE); break;
    case TASKMGR_ACTION_STARTUP: select_page(TASKMGR_TAB_STARTUP); break;
    case TASKMGR_ACTION_ENABLE: toggle_selected_startup_entry(); break;
    case TASKMGR_ACTION_REMOVE: remove_selected_startup_entry(); break;
    case TASKMGR_ACTION_SERVICE_MANAGER: open_service_manager(); break;
    default: break;
    }
}

void action_callback(Fl_Widget *, void *data)
{
    perform_action((uint32_t)(uintptr_t)data);
}

class TaskBrowser : public Fl_Browser {
    int startup_;
public:
    TaskBrowser(int x, int y, int w, int h, int startup, const char *label = NULL)
        : Fl_Browser(x, y, w, h, label), startup_(startup) {}
    int handle(int event) override
    {
        if (event == FL_KEYDOWN) {
            int key = Fl::event_key();
            if (key == FL_F + 5) {
                refresh_all();
                return 1;
            }
            if (key == FL_Delete) {
                if (startup_) remove_selected_startup_entry();
                else kill_selected_task();
                return 1;
            }
            if (!startup_ && (key == FL_Enter || key == FL_KP_Enter)) {
                show_task_details();
                return 1;
            }
        }
        int result = Fl_Browser::handle(event);
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && !startup_) {
            int row = value();
            if (row > 0 && (uint32_t)row <= tree_count)
                selected_pid = tasks[tree_rows[row - 1].index].pid;
            else selected_pid = 0;
            update_controls();
            if (process_popup) {
                const Fl_Menu_Item *picked = process_popup->popup();
                if (picked) perform_action((uint32_t)(uintptr_t)picked->user_data());
            }
            return 1;
        }
        return result;
    }
};

void process_selected(Fl_Widget *widget, void *)
{
    if (updating) return;
    Fl_Browser *browser = (Fl_Browser *)widget;
    int row = browser->value();
    if (row < 1 || (uint32_t)row > tree_count) selected_pid = 0;
    else selected_pid = tasks[tree_rows[row - 1].index].pid;
    update_controls();
}

void startup_selected(Fl_Widget *widget, void *)
{
    if (updating) return;
    int row = ((Fl_Browser *)widget)->value();
    if (row < 1 || (uint32_t)row > startup_entry_count) {
        selected_startup = -1;
        selected_startup_id = 0;
    } else {
        selected_startup = row - 1;
        selected_startup_id = startup_entries[selected_startup].id;
    }
    update_controls();
}

void startup_user_selected(Fl_Widget *widget, void *)
{
    if (updating) return;
    int row = ((Fl_Choice *)widget)->value();
    if (row < 0 || (uint32_t)row >= startup_user_count) return;
    startup_selected_uid = startup_users[row].uid;
    selected_startup = -1;
    selected_startup_id = 0;
    refresh_startup_choice();
    refresh_startup_entries();
    update_controls();
}

void tabs_changed(Fl_Widget *widget, void *)
{
    Fl_Tabs *tabs = (Fl_Tabs *)widget;
    Fl_Widget *page = tabs->value();
    if (page == page_processes) active_tab = TASKMGR_TAB_PROCESSES;
    else if (page == page_performance) active_tab = TASKMGR_TAB_PERFORMANCE;
    else if (page == page_startup) {
        active_tab = TASKMGR_TAB_STARTUP;
        refresh_startup();
    }
    update_controls();
}

void refresh_all()
{
    refresh_performance();
    refresh_tasks();
    if (active_tab == TASKMGR_TAB_STARTUP) refresh_startup();
    update_controls();
    if (performance_widget) performance_widget->redraw();
}

void timer_callback(void *)
{
    refresh_all();
    Fl::repeat_timeout(0.5, timer_callback);
}

void add_menu_action(const char *group, const char *label, uint32_t action, int divider = 0)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", group, label);
    menu_bar->add(path, 0, action_callback, (void *)(uintptr_t)action,
                  divider ? FL_MENU_DIVIDER : 0);
}

void build_ui()
{
    shell = new Fl_Double_Window(1000, 740, T("Task Manager"));
    shell->size_range(760, 520);
    menu_bar = new Fl_Menu_Bar(0, 0, 1000, 30);
    add_menu_action(T("File"), T("Refresh"), TASKMGR_ACTION_REFRESH);
    add_menu_action(T("File"), T("End Task"), TASKMGR_ACTION_END);
    add_menu_action(T("File"), T("Details"), TASKMGR_ACTION_DETAILS, 1);
    add_menu_action(T("File"), T("Open Service Manager"), TASKMGR_ACTION_SERVICE_MANAGER);
    add_menu_action(T("File"), T("About"), TASKMGR_ACTION_ABOUT);
    add_menu_action(T("Options"), T("Processes"), TASKMGR_ACTION_PROCESSES);
    add_menu_action(T("Options"), T("Performance"), TASKMGR_ACTION_PERFORMANCE);
    add_menu_action(T("Options"), T("Service Manager"), TASKMGR_ACTION_STARTUP);
    add_menu_action(T("Options"), T("Open Service Manager"), TASKMGR_ACTION_SERVICE_MANAGER, 1);
    add_menu_action(T("Options"), T("About"), TASKMGR_ACTION_ABOUT);

    Fl_Button *refresh = new Fl_Button(8, 34, 88, 30, T("Refresh"));
    refresh->callback(action_callback, (void *)(uintptr_t)TASKMGR_ACTION_REFRESH);
    button_end_task = new Fl_Button(102, 34, 104, 30, T("End Task"));
    button_end_task->callback(action_callback, (void *)(uintptr_t)TASKMGR_ACTION_END);
    Fl_Button *details = new Fl_Button(212, 34, 88, 30, T("Details"));
    details->callback(action_callback, (void *)(uintptr_t)TASKMGR_ACTION_DETAILS);
    Fl_Button *open_manager = new Fl_Button(306, 34, 202, 30, T("Open Service Manager"));
    open_manager->callback(action_callback,
                           (void *)(uintptr_t)TASKMGR_ACTION_SERVICE_MANAGER);

    status_output = new Fl_Output(8, 704, 984, 26);
    status_output->value(T("Ready"));
    tab_stack = new Fl_Tabs(8, 72, 984, 624);
    tab_stack->callback(tabs_changed);

    page_processes = new Fl_Group(8, 94, 984, 602, T("Processes"));
    page_processes->begin();
    Fl_Box *columns = new Fl_Box(16, 100, 960, 26,
        T("Process tree: name, PID, CPU, memory, state, user and privileges"));
    columns->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    process_list = new TaskBrowser(16, 128, 960, 550, 0);
    process_list->format_char(0);
    process_list->when(FL_WHEN_CHANGED);
    process_list->callback(process_selected);
    page_processes->resizable(process_list);
    process_list->take_focus();
    page_processes->end();

    page_performance = new Fl_Group(8, 94, 984, 602, T("Performance"));
    page_performance->begin();
    PerformanceView *perf = new PerformanceView(16, 100, 960, 576);
    performance_widget = perf;
    page_performance->resizable(perf);
    page_performance->end();

    page_startup = new Fl_Group(8, 94, 984, 602, T("Service Manager"));
    page_startup->begin();
    Fl_Box *user_label = new Fl_Box(16, 102, 64, 28, T("User"));
    user_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    startup_choice = new Fl_Choice(82, 102, 140, 28);
    startup_choice->callback(startup_user_selected);
    startup_username = new Fl_Output(232, 102, 260, 28);
    Fl_Button *launch_manager = new Fl_Button(714, 102, 262, 30,
        T("Open Service Manager"));
    launch_manager->callback(action_callback,
                             (void *)(uintptr_t)TASKMGR_ACTION_SERVICE_MANAGER);
    button_startup_toggle = new Fl_Button(16, 140, 96, 30, T("Enable"));
    button_startup_toggle->callback(action_callback,
                                    (void *)(uintptr_t)TASKMGR_ACTION_ENABLE);
    button_startup_remove = new Fl_Button(120, 140, 96, 30, T("Remove"));
    button_startup_remove->callback(action_callback,
                                    (void *)(uintptr_t)TASKMGR_ACTION_REMOVE);
    Fl_Box *startup_heading = new Fl_Box(228, 140, 748, 30, T("Startup applications"));
    startup_heading->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    startup_list_widget = new TaskBrowser(16, 176, 960, 500, 1);
    startup_list_widget->format_char(0);
    startup_list_widget->when(FL_WHEN_CHANGED);
    startup_list_widget->callback(startup_selected);
    page_startup->resizable(startup_list_widget);
    page_startup->end();

    tab_stack->end();
    process_popup = new Fl_Menu_Button(0, 0, 0, 0);
    process_popup->type(Fl_Menu_Button::POPUP3);
    process_popup->add(T("End Task"), 0, NULL,
                       (void *)(uintptr_t)TASKMGR_ACTION_END);
    process_popup->add(T("Details"), 0, NULL,
                       (void *)(uintptr_t)TASKMGR_ACTION_DETAILS);
    process_popup->add(T("Refresh"), 0, NULL,
                       (void *)(uintptr_t)TASKMGR_ACTION_REFRESH);
    shell->resizable(tab_stack);
    shell->end();
    reliefos_fltk::style(shell);
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    reliefos_fltk::init();
    taskmgr_perf_history_init(&history);
    build_ui();
    shell->show(argc, argv);
    refresh_performance();
    refresh_tasks();
    refresh_startup();
    update_controls();
    puts("[taskmgr.elf] FLTK task manager ready");
    fflush(stdout);
    Fl::add_timeout(0.5, timer_callback);
    return Fl::run();
}
