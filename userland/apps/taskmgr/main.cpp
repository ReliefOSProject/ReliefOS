extern "C" {
#include "model.h"
#include "gpu_sample.h"
#include <reliefos/auth.h>
#include <reliefos/system.h>
#include <reliefos/launch.h>
#include <reliefos/system.h>
}

#include "fltk_support.h"
#include "autostart.h"
#include "startup_page.h"

#include <FL/Fl_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
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
};

class TaskWindow;

TaskWindow *shell;
Fl_Menu_Bar *menu_bar;
Fl_Tabs *tab_stack;
Fl_Group *page_processes;
Fl_Group *page_performance;
StartupPage *page_startup;
Fl_Browser *process_list;
Fl_Box *process_headers[7];
Fl_Output *status_output;
Fl_Button *button_refresh;
Fl_Button *button_end_task;
Fl_Button *button_details;
Fl_Menu_Button *process_popup;
Fl_Widget *performance_widget;
class PerformanceView;
PerformanceView *performance_view;
Fl_Scroll *performance_scroll;

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

int active_tab = TASKMGR_TAB_PROCESSES;
int updating;
int process_column_widths[8];

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
void layout_ui(int width, int height);
void update_performance_layout();

class TaskWindow : public Fl_Double_Window {
public:
    TaskWindow(int width, int height, const char *label)
        : Fl_Double_Window(width, height, label) {}
    void resize(int x, int y, int width, int height) override;
};

void set_status(const char *text)
{
    if (status_output) status_output->value(text ? text : "");
}

void sanitize_column_text(char *destination, size_t capacity, const char *source,
                          size_t source_limit = (size_t)-1)
{
    if (!destination || !capacity) return;
    size_t used = 0;
    for (size_t i = 0; source && source[i] && i < source_limit && used + 1 < capacity; ++i) {
        unsigned char byte = (unsigned char)source[i];
        destination[used++] = byte < 0x20U || byte == 0x7fU ? ' ' : (char)byte;
    }
    destination[used] = 0;
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

void update_controls()
{
    if (button_end_task) {
        if (active_tab == TASKMGR_TAB_PROCESSES && selected_task_killable())
            button_end_task->activate();
        else button_end_task->deactivate();
    }
    if (button_details) {
        if (active_tab == TASKMGR_TAB_PROCESSES && selected_task())
            button_details->activate();
        else button_details->deactivate();
    }

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
            char name[RELIEFOS_TASK_NAME_LEN + 1], user[RELIEFOS_TASK_NAME_LEN + 1];
            char pid[16], cpu[16], mem[24], state[32], privileges[32];
            taskmgr_format_dec(pid, sizeof(pid), tasks[index].pid);
            taskmgr_format_percent(cpu, sizeof(cpu), task_cpu_percent[index]);
            taskmgr_format_memory(mem, sizeof(mem), tasks[index].memory_kib);
            sanitize_column_text(name, sizeof(name), tasks[index].name,
                                 sizeof(tasks[index].name));
            sanitize_column_text(user, sizeof(user), task_user_name(&tasks[index]));
            sanitize_column_text(state, sizeof(state), taskmgr_state_name(tasks[index].state));
            sanitize_column_text(privileges, sizeof(privileges),
                                 task_privilege_name(&tasks[index]));
            unsigned indent = tree_rows[i].depth * 2U;
            if (indent > 120U) indent = 120U;
            snprintf(lines[i], sizeof(lines[i]), "%*s%s\t%s\t%s\t%s\t%s\t%s\t%s",
                     (int)indent, "", name, pid, cpu, mem, state, user, privileges);
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

void refresh_startup()
{
    if (page_startup) page_startup->refresh();
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
        update_performance_layout();
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
    update_performance_layout();
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
    fl_color(available ? FL_FOREGROUND_COLOR : FL_INACTIVE_COLOR);
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

void update_performance_layout()
{
    if (!performance_scroll || !performance_view) return;
    int view_width = performance_scroll->w() - 18;
    if (view_width < 1) view_width = 1;
    int viewport_height = performance_scroll->h();
    if (viewport_height < 1) viewport_height = 1;

    uint32_t cpu_count = perf_info.cpu_count > RELIEFOS_PERF_MAX_CPUS
                             ? RELIEFOS_PERF_MAX_CPUS : perf_info.cpu_count;
    int content_width = view_width - 24;
    int core_columns = cpu_count > 16U ? 3 : (cpu_count > 8U ? 2 : 1);
    if (content_width < 200) core_columns = 1;
    int core_rows = (int)((cpu_count + (uint32_t)core_columns - 1U) /
                          (uint32_t)core_columns);

    int graph_height = viewport_height > 440 ? 126 : 100;
    int required_height = graph_height + 168 + core_rows * 27;
    int content_height = viewport_height > required_height
                             ? viewport_height : required_height;
    if (content_height > 440) graph_height = 126;
    else graph_height = 100;
    required_height = graph_height + 168 + core_rows * 27;
    content_height = viewport_height > required_height
                         ? viewport_height : required_height;
    if (!perf_valid) content_height = viewport_height;

    if (performance_view->w() != view_width || performance_view->h() != content_height) {
        int old_x = performance_scroll->xposition();
        int old_y = performance_scroll->yposition();
        // Fl_Scroll moves its child to encode the current offset. Keep that
        // position when changing content size; moving it to the viewport
        // origin would desynchronize the child and the scrollbar value.
        performance_view->resize(performance_view->x(), performance_view->y(),
                                 view_width, content_height);
        int max_y = content_height - viewport_height;
        if (old_y > max_y) old_y = max_y;
        if (old_y < 0) old_y = 0;
        performance_scroll->scroll_to(old_x, old_y);
        performance_scroll->redraw();
    }
}

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
    const uint32_t pid = task->pid;
    char prompt[RELIEFOS_TASK_NAME_LEN + 96];
    snprintf(prompt, sizeof(prompt), T("End task %s (PID %u)?"), task->name, pid);
    if (!confirm_dialog(T("End Task"), prompt)) return;
    // The modal dialog runs the sampling timer; its next snapshot can
    // replace tasks[]. Act on the PID named in the confirmation.
    if (reliefos_task_kill(pid) < 0) {
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
    case TASKMGR_ACTION_REFRESH:
        refresh_all();
        if (active_tab == TASKMGR_TAB_STARTUP) page_startup->refresh(true);
        break;
    case TASKMGR_ACTION_END: kill_selected_task(); break;
    case TASKMGR_ACTION_DETAILS: show_task_details(); break;
    case TASKMGR_ACTION_ABOUT: show_about(); break;
    case TASKMGR_ACTION_PROCESSES: select_page(TASKMGR_TAB_PROCESSES); break;
    case TASKMGR_ACTION_PERFORMANCE: select_page(TASKMGR_TAB_PERFORMANCE); break;
    case TASKMGR_ACTION_STARTUP: select_page(TASKMGR_TAB_STARTUP); break;
    default: break;
    }
}

void action_callback(Fl_Widget *, void *data)
{
    perform_action((uint32_t)(uintptr_t)data);
}

class TaskBrowser : public Fl_Browser {
public:
    TaskBrowser(int x, int y, int w, int h, const char *label = NULL)
        : Fl_Browser(x, y, w, h, label)
    {
        type(FL_HOLD_BROWSER);
    }
    int handle(int event) override
    {
        if (event == FL_KEYDOWN) {
            int key = Fl::event_key();
            if (key == FL_F + 5) {
                refresh_all();
                return 1;
            }
            if (key == FL_Delete) {
                kill_selected_task();
                return 1;
            }
            if ((key == FL_Enter || key == FL_KP_Enter)) {
                show_task_details();
                return 1;
            }
        }
        int result = Fl_Browser::handle(event);
        if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
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

void layout_ui(int width, int height)
{
    if (!shell || !menu_bar || !tab_stack) return;
    if (width < 1 || height < 1) return;

    menu_bar->resize(0, 0, width, 28);
    const int status_y = height - 32;
    if (status_output) status_output->resize(8, status_y, width - 16, 24);

    const int tabs_height = height - 74;
    tab_stack->resize(12, 34, width - 24, tabs_height);

    const int page_x = 12;
    const int page_y = 56;
    const int page_width = width - 24;
    const int page_height = tabs_height - 22;
    if (page_processes) page_processes->resize(page_x, page_y, page_width, page_height);
    if (page_performance) page_performance->resize(page_x, page_y, page_width, page_height);
    if (page_startup) page_startup->resize(page_x, page_y, page_width, page_height);

    if (button_refresh) button_refresh->resize(20, 66, 88, 30);
    if (button_details) button_details->resize(112, 66, 96, 30);
    if (button_end_task) button_end_task->resize(212, 66, 104, 30);
    if (process_list) {
        const int list_x = 20;
        const int list_y = 132;
        const int list_width = width - 40;
        const int list_height = height - 180;
        process_list->resize(list_x, list_y, list_width, list_height);

        int available = list_width - 20;
        if (available < 1) available = 1;
        process_column_widths[0] = available - (80 + 64 + 96 + 84 + 110 + 90);
        if (process_column_widths[0] < 150) process_column_widths[0] = 150;
        process_column_widths[1] = 80;
        process_column_widths[2] = 64;
        process_column_widths[3] = 96;
        process_column_widths[4] = 84;
        process_column_widths[5] = 110;
        process_column_widths[6] = 90;
        process_column_widths[7] = 0;
        process_list->column_widths(process_column_widths);
        int column_x = list_x + 5;
        for (int i = 0; i < 7; ++i) {
            if (process_headers[i])
                process_headers[i]->resize(column_x, 108,
                                           process_column_widths[i], 22);
            column_x += process_column_widths[i];
        }
    }

    if (performance_scroll) {
        performance_scroll->resize(20, 66, width - 40, height - 114);
        update_performance_layout();
    }

    if (process_list) process_list->redraw();
    shell->redraw();
}

void TaskWindow::resize(int x, int y, int width, int height)
{
    Fl_Double_Window::resize(x, y, width, height);
    layout_ui(width, height);
}

void build_ui()
{
    shell = new TaskWindow(1000, 660, T("Task Manager"));
    shell->size_range(800, 520);
    menu_bar = new Fl_Menu_Bar(0, 0, 1000, 28);
    add_menu_action(T("File"), T("Refresh"), TASKMGR_ACTION_REFRESH);
    add_menu_action(T("File"), T("End Task"), TASKMGR_ACTION_END);
    add_menu_action(T("File"), T("Details"), TASKMGR_ACTION_DETAILS, 1);
    add_menu_action(T("File"), T("About"), TASKMGR_ACTION_ABOUT);
    add_menu_action(T("Options"), T("Processes"), TASKMGR_ACTION_PROCESSES);
    add_menu_action(T("Options"), T("Performance"), TASKMGR_ACTION_PERFORMANCE);
    add_menu_action(T("Options"), T("Startup applications"), TASKMGR_ACTION_STARTUP);

    status_output = new Fl_Output(8, 628, 984, 24);
    status_output->value(T("Ready"));
    tab_stack = new Fl_Tabs(12, 34, 976, 586);
    tab_stack->callback(tabs_changed);

    page_processes = new Fl_Group(12, 56, 976, 564, T("Processes"));
    page_processes->tooltip(T("Inspect running tasks and open details or end a task."));
    page_processes->begin();
    button_refresh = new Fl_Button(20, 66, 88, 30, T("Refresh"));
    button_refresh->callback(action_callback, (void *)(uintptr_t)TASKMGR_ACTION_REFRESH);
    button_refresh->tooltip(T("Refresh task and performance data now."));
    button_details = new Fl_Button(112, 66, 96, 30, T("Details"));
    button_details->callback(action_callback, (void *)(uintptr_t)TASKMGR_ACTION_DETAILS);
    button_details->tooltip(T("Show details for the selected task."));
    button_end_task = new Fl_Button(212, 66, 104, 30, T("End Task"));
    button_end_task->callback(action_callback, (void *)(uintptr_t)TASKMGR_ACTION_END);
    button_end_task->tooltip(T("End the selected task after confirmation."));

    const char *process_column_labels[7] = {
        T("Process"), T("PID"), T("CPU"), T("Memory"), T("State"), T("User"), T("Privileges")
    };
    for (int i = 0; i < 7; ++i) {
        process_headers[i] = new Fl_Box(25, 108, 100, 22, process_column_labels[i]);
        process_headers[i]->align((i == 0 ? FL_ALIGN_LEFT : FL_ALIGN_CENTER) |
                                  FL_ALIGN_INSIDE);
    }
    process_list = new TaskBrowser(20, 132, 960, 480);
    process_list->format_char(0);
    process_list->column_char('\t');
    process_list->when(FL_WHEN_CHANGED);
    process_list->callback(process_selected);
    process_list->tooltip(T("Select a row; press Enter for details or Delete to end the task."));
    page_processes->resizable(process_list);
    process_list->take_focus();
    page_processes->end();

    page_performance = new Fl_Group(12, 56, 976, 564, T("Performance"));
    page_performance->tooltip(T("View live CPU, memory, and estimated GPU usage."));
    page_performance->begin();
    performance_scroll = new Fl_Scroll(20, 66, 960, 546);
    performance_scroll->type(Fl_Scroll::VERTICAL);
    performance_scroll->box(FL_NO_BOX);
    performance_scroll->tooltip(T("Scroll to see per-core CPU usage and memory totals."));
    performance_scroll->begin();
    PerformanceView *perf = new PerformanceView(20, 66, 942, 546);
    perf->tooltip(T("Live CPU, memory, and estimated GPU history with per-core usage."));
    performance_view = perf;
    performance_widget = perf;
    performance_scroll->end();
    page_performance->resizable(performance_scroll);
    page_performance->end();

    page_startup = new StartupPage(12, 56, 976, 564, set_status);

    tab_stack->end();
    process_popup = new Fl_Menu_Button(0, 0, 0, 0);
    process_popup->type(Fl_Menu_Button::POPUP3);
    process_popup->add(T("End Task"), 0, NULL,
                       (void *)(uintptr_t)TASKMGR_ACTION_END);
    process_popup->add(T("Details"), 0, NULL,
                       (void *)(uintptr_t)TASKMGR_ACTION_DETAILS);
    process_popup->add(T("Refresh"), 0, NULL,
                       (void *)(uintptr_t)TASKMGR_ACTION_REFRESH);
    // Geometry is assigned by layout_ui; do not also scale children around
    // the old status-bar rectangle through Fl_Group's automatic layout.
    shell->resizable(NULL);
    shell->end();
    reliefos_fltk::style(shell);
    for (int i = 0; i < 7; ++i)
        if (process_headers[i]) process_headers[i]->labelsize(13);
    layout_ui(shell->w(), shell->h());
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--run-autostart")) return autostart::run_session();
    reliefos_fltk::init("taskmgr");
    // Configuration workers fork from this UI process. Use FLTK's file dialogs.
    Fl::option(Fl::OPTION_FNFC_USES_GTK, false);
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
