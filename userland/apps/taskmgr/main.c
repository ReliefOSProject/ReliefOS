#include "model.h"
#include "gpu_sample.h"

#include <libintl.h>
#include <locale.h>
#include <reliefos/auth.h>
#include <reliefos/gui.h>
#include <reliefos/layout.h>
#include <reliefos/startup.h>
#include <reliefos/system.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <Xm/CascadeB.h>
#include <Xm/ComboBox.h>
#include <Xm/DrawingA.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/List.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/TabStack.h>

#define T(s) gettext(s)

enum {
    TASKMGR_TAB_PROCESSES = 0,
    TASKMGR_TAB_PERFORMANCE = 1,
    TASKMGR_TAB_STARTUP = 2,
};

static XtAppContext app;
static Widget shell, status_label;
static Widget tab_stack, page_processes, page_performance, page_startup;
static Widget process_list, process_popup;
static Widget perf_area;
static Widget startup_combo, startup_list_widget, startup_user_label;
static Widget button_end_task, button_startup_toggle, button_startup_remove;
static Widget menu_end_task, popup_end_task, popup_details;

static struct reliefos_task_info tasks[RELIEFOS_TASK_MAX];
static struct reliefos_task_info previous_tasks[RELIEFOS_TASK_MAX];
static uint32_t task_cpu_percent[RELIEFOS_TASK_MAX];
static uint32_t previous_task_cpu_percent[RELIEFOS_TASK_MAX];
static uint32_t task_count, previous_task_count;
static uint64_t previous_sample_total;
static struct taskmgr_tree_row tree_rows[RELIEFOS_TASK_MAX];
static uint32_t tree_count;
static uint32_t selected_pid;

static struct taskmgr_perf_history history;
static struct reliefos_perf_info perf_info;
static uint64_t last_busy_ticks, last_idle_ticks;
static uint64_t last_cpu_busy[RELIEFOS_PERF_MAX_CPUS];
static uint64_t last_cpu_idle[RELIEFOS_PERF_MAX_CPUS];
static uint32_t cpu_percent_by_core[RELIEFOS_PERF_MAX_CPUS];
static uint32_t cpu_percent, mem_percent;
static int cpu_snapshot_valid, perf_valid;
static struct taskmgr_gpu_sample gpu_sample;

static struct reliefos_startup_entry startup_entries[RELIEFOS_STARTUP_MAX_ENTRIES];
static uint32_t startup_entry_count;
static struct reliefos_user_info startup_users[16];
static uint32_t startup_user_count, startup_selected_uid;
static int startup_available;
static int selected_startup = -1;
static int active_tab = TASKMGR_TAB_PROCESSES;
static int updating;

/* Performance plotting state; the GC and colors are created once the
 * drawing area has a window, like any other X11 client. */
static GC perf_gc;
static Font perf_font;
static unsigned long perf_pixels[256];
static unsigned long color_white, color_border, color_muted, color_text, color_accent;

#define TASKMGR_CORE_PALETTE_SIZE 32U
static const uint32_t core_palette[TASKMGR_CORE_PALETTE_SIZE] = {
    0x00B81C1C, 0x001CB89E, 0x00000080, 0x007AB800,
    0x009E1CB8, 0x0000B81F, 0x00B8A753, 0x003737B8,
    0x00B87A00, 0x005C8039, 0x00006699, 0x006D1380,
    0x00804000, 0x00B8005C, 0x005396B8, 0x00B85396,
    0x00158000, 0x0074B853, 0x00008040, 0x00800000,
    0x00800040, 0x00B8B800, 0x005C00B8, 0x003DB800,
    0x0000B85C, 0x008553B8, 0x00B86237, 0x001F00B8,
    0x00398068, 0x00263580, 0x006A8000, 0x00803939,
};

static unsigned long alloc_color(Display *display, uint32_t rgb)
{
    XColor color;
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    color.red = (unsigned short)(((rgb >> 16) & 0xffu) * 0x0101u);
    color.green = (unsigned short)(((rgb >> 8) & 0xffu) * 0x0101u);
    color.blue = (unsigned short)((rgb & 0xffu) * 0x0101u);
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, colormap, &color)) {
        return BlackPixel(display, DefaultScreen(display));
    }
    return color.pixel;
}

static unsigned long core_color(uint32_t core)
{
    return perf_pixels[core % TASKMGR_CORE_PALETTE_SIZE];
}

static void set_label(Widget widget, const char *text)
{
    XmString label = XmStringCreateLocalized((char *)text);
    XtVaSetValues(widget, XmNlabelString, label, NULL);
    XmStringFree(label);
}

static void set_status(const char *text)
{
    if (status_label) set_label(status_label, text);
}

static int task_index_by_pid(uint32_t pid)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i].pid == pid) return (int)i;
    }
    return -1;
}

static int previous_index_by_pid(uint32_t pid)
{
    for (uint32_t i = 0; i < previous_task_count; ++i) {
        if (previous_tasks[i].pid == pid) return (int)i;
    }
    return -1;
}

static const char *task_user_name(const struct reliefos_task_info *task)
{
    if (task && task->username[0]) return task->username;
    return task && task->uid ? T("Unknown") : T("System");
}

static const char *task_privilege_name(const struct reliefos_task_info *task)
{
    if (!task || !task->uid) return T("System");
    if (task->flags & RELIEFOS_TASK_SNAPSHOT_FLAG_ELEVATED_ADMIN) return T("Elevated");
    if (task->role == RELIEFOS_AUTH_ROLE_ADMIN) return T("Admin");
    return T("Standard");
}

static struct reliefos_task_info *selected_task(void)
{
    int index = selected_pid ? task_index_by_pid(selected_pid) : -1;
    return index >= 0 ? &tasks[index] : 0;
}

static int selected_task_killable(void)
{
    struct reliefos_task_info *task = selected_task();
    if (!task) return 0;
    return taskmgr_task_killable(task->pid, (uint32_t)getpid(), task->kind,
                                 task->state, task->flags);
}

static void startup_command_line(char *text, uint32_t cap,
                                 const struct reliefos_startup_command *command)
{
    uint32_t used = 0;
    text[0] = 0;
    for (uint32_t i = 0; command && command->path[i] && used + 1 < cap; ++i) {
        text[used++] = command->path[i];
    }
    for (uint32_t a = 0; command && a < command->argc; ++a) {
        if (used + 1 < cap) text[used++] = ' ';
        for (uint32_t i = 0; command->args[a][i] && used + 1 < cap; ++i) {
            text[used++] = command->args[a][i];
        }
    }
    text[used] = 0;
}

static void update_controls(void)
{
    struct reliefos_task_info *task = selected_task();
    struct reliefos_startup_entry *entry = (active_tab == TASKMGR_TAB_STARTUP &&
                                            selected_startup >= 0 &&
                                            (uint32_t)selected_startup < startup_entry_count)
                                               ? &startup_entries[selected_startup]
                                               : 0;
    if (button_end_task) XtSetSensitive(button_end_task, selected_task_killable());
    if (menu_end_task) XtSetSensitive(menu_end_task, selected_task_killable());
    if (popup_end_task) XtSetSensitive(popup_end_task, selected_task_killable());
    if (popup_details) XtSetSensitive(popup_details, task != 0);
    if (button_startup_toggle) {
        set_label(button_startup_toggle, entry && entry->enabled ? T("Disable")
                                                                : T("Enable"));
        XtSetSensitive(button_startup_toggle, entry != 0 && startup_available);
    }
    if (button_startup_remove) {
        XtSetSensitive(button_startup_remove, entry != 0 && startup_available);
    }
}

/* Refresh one XmList from formatted rows while keeping the visible top row
 * and the selection stable, so the 500 ms refresh never jumps the view. */
static void list_rebuild(Widget list, const char **rows, uint32_t count,
                         int select_position)
{
    int top = 1;
    XtVaGetValues(list, XmNtopItemPosition, &top, NULL);
    updating = 1;
    XmListDeleteAllItems(list);
    for (uint32_t i = 0; i < count; ++i) {
        XmString item = XmStringCreateLocalized((char *)rows[i]);
        XmListAddItemUnselected(list, item, 0);
        XmStringFree(item);
    }
    if (select_position > 0 && (uint32_t)select_position <= count) {
        XmListSelectPos(list, select_position, False);
    }
    if (top <= (int)count) XmListSetPos(list, top);
    updating = 0;
}

static void refresh_tasks(void)
{
    char lines[RELIEFOS_TASK_MAX][192];
    const char *rows[RELIEFOS_TASK_MAX];
    uint32_t pids[RELIEFOS_TASK_MAX];
    uint32_t parents[RELIEFOS_TASK_MAX];
    uint64_t next_tick = 0;
    uint64_t sample_total;
    uint64_t tick_delta = 0;
    int select_position = 0;
    int count = reliefos_task_snapshot(tasks, RELIEFOS_TASK_MAX, &next_tick);

    task_count = count > 0 ? (uint32_t)count : 0;
    sample_total = perf_info.busy_ticks + perf_info.idle_ticks;
    if (previous_sample_total && sample_total > previous_sample_total) {
        tick_delta = sample_total - previous_sample_total;
    }
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
    for (uint32_t i = 0; i < tree_count; ++i) {
        uint32_t index = tree_rows[i].index;
        char pid[16], cpu[16], mem[24];
        taskmgr_format_dec(pid, sizeof(pid), tasks[index].pid);
        taskmgr_format_percent(cpu, sizeof(cpu), task_cpu_percent[index]);
        taskmgr_format_memory(mem, sizeof(mem), tasks[index].memory_kib);
        snprintf(lines[i], sizeof(lines[i]),
                 "%*s%-14.14s %7s %6s %9s %-5s %-10s %-8s",
                 (int)(tree_rows[i].depth * 2U), "", tasks[index].name,
                 pid, cpu, mem, taskmgr_state_name(tasks[index].state),
                 task_user_name(&tasks[index]),
                 task_privilege_name(&tasks[index]));
        rows[i] = lines[i];
        if (selected_pid && tasks[index].pid == selected_pid) {
            select_position = (int)i + 1;
        }
    }
    list_rebuild(process_list, rows, tree_count, select_position);
    previous_task_count = task_count;
    previous_sample_total = sample_total;
    for (uint32_t i = 0; i < task_count; ++i) {
        previous_task_cpu_percent[i] = task_cpu_percent[i];
        previous_tasks[i] = tasks[i];
    }
}

static void refresh_startup_users(void)
{
    struct reliefos_user_info current;
    struct reliefos_user_info *allocated = 0;
    uint32_t count = 0;

    startup_user_count = 0;
    current = (struct reliefos_user_info){0};
    if (reliefos_auth_current(&current) < 0) {
        startup_available = 0;
        return;
    }
    if (current.role == RELIEFOS_AUTH_ROLE_ADMIN &&
        reliefos_auth_users_alloc(&allocated, 0, &count) == 0 &&
        count && count <= sizeof(startup_users) / sizeof(startup_users[0])) {
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

static void refresh_startup_combo(void)
{
    /* The user list changes rarely; only touch the items when it does so a
     * half-open dropdown is never yanked out of the user's hands. */
    static uint32_t built_count;
    static uint32_t built_uids[16];
    int changed = built_count != startup_user_count;
    for (uint32_t i = 0; !changed && i < startup_user_count; ++i) {
        if (built_uids[i] != startup_users[i].uid) changed = 1;
    }
    if (!changed) return;
    updating = 1;
    for (uint32_t i = 0; i < built_count; ++i) XmComboBoxDeletePos(startup_combo, 1);
    for (uint32_t i = 0; i < startup_user_count; ++i) {
        XmString name = XmStringCreateLocalized(startup_users[i].username);
        XmComboBoxAddItem(startup_combo, name, XmLAST_POSITION, False);
        if (startup_users[i].uid == startup_selected_uid) XmComboBoxSetItem(startup_combo, name);
        XmStringFree(name);
        built_uids[i] = startup_users[i].uid;
    }
    built_count = startup_user_count;
    updating = 0;
}

static void refresh_startup_entries(void)
{
    char lines[RELIEFOS_STARTUP_MAX_ENTRIES][528];
    const char *rows[RELIEFOS_STARTUP_MAX_ENTRIES];
    uint32_t count = 0;
    int select_position = selected_startup >= 0 ? selected_startup + 1 : 0;

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
    for (uint32_t i = 0; i < count; ++i) {
        char command[448];
        startup_command_line(command, sizeof(command), &startup_entries[i].command);
        snprintf(lines[i], sizeof(lines[i]), "%-8s  %s",
                 startup_entries[i].enabled ? T("Enabled") : T("Disabled"),
                 command);
        rows[i] = lines[i];
    }
    if (select_position > (int)count) select_position = 0;
    list_rebuild(startup_list_widget, rows, count, select_position);
    if (select_position) selected_startup = select_position - 1;
    else selected_startup = -1;
}

static void refresh_startup(void)
{
    refresh_startup_users();
    refresh_startup_combo();
    refresh_startup_entries();
    update_controls();
}

static void redraw_performance(void)
{
    if (!perf_area || !XtIsRealized(perf_area)) return;
    XClearArea(XtDisplay(perf_area), XtWindow(perf_area), 0, 0, 0, 0, True);
}

static void refresh_performance(void)
{
    struct reliefos_perf_info next;
    gpu_sdk_info_t next_gpu = {
        .size = sizeof(gpu_sdk_info_t),
        .version = GPU_SDK_ABI_VERSION,
    };
    int gpu_result = gpu_sdk_info(&next_gpu);
    uint32_t slot;
    uint32_t cpu_count;

    if (taskmgr_gpu_sample_update(&gpu_sample, gpu_result < 0 ? 0 : &next_gpu)) {
        taskmgr_perf_history_clear_gpu(&history);
    }
    if (reliefos_perf_info(&next) < 0) {
        perf_valid = 0;
        set_status(T("Performance data unavailable"));
        return;
    }
    cpu_percent = taskmgr_busy_percent(last_busy_ticks, last_idle_ticks,
                                       next.busy_ticks, next.idle_ticks,
                                       cpu_snapshot_valid, cpu_percent);
    mem_percent = taskmgr_mem_percent(next.total_memory_kib, next.free_memory_kib);
    slot = taskmgr_perf_history_push(&history, mem_percent,
                                     gpu_sample.percent, gpu_sample.valid);
    cpu_count = next.cpu_count;
    if (cpu_count > RELIEFOS_PERF_MAX_CPUS) cpu_count = RELIEFOS_PERF_MAX_CPUS;
    for (uint32_t i = 0; i < RELIEFOS_PERF_MAX_CPUS; ++i) {
        if (i >= cpu_count || !next.cpus[i].online) {
            cpu_percent_by_core[i] = 0;
            /* Offline cores have no signal to plot; leave a gap. */
            history.core[i][slot] = TASKMGR_PERF_MISSING;
        } else {
            cpu_percent_by_core[i] = taskmgr_busy_percent(
                last_cpu_busy[i], last_cpu_idle[i], next.cpus[i].busy_ticks,
                next.cpus[i].idle_ticks, cpu_snapshot_valid,
                cpu_percent_by_core[i]);
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

static void refresh_all(void)
{
    refresh_performance();
    refresh_tasks();
    if (active_tab == TASKMGR_TAB_STARTUP) refresh_startup();
    update_controls();
    redraw_performance();
}

static void draw_text(int x, int y, const char *text)
{
    if (!text) return;
    XDrawString(XtDisplay(perf_area), XtWindow(perf_area), perf_gc, x, y,
                text, (int)strlen(text));
}

static void draw_fill(int x, int y, int w, int h, unsigned long pixel)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(XtDisplay(perf_area), perf_gc, pixel);
    XFillRectangle(XtDisplay(perf_area), XtWindow(perf_area), perf_gc, x, y,
                   (unsigned)w, (unsigned)h);
    XSetForeground(XtDisplay(perf_area), perf_gc, color_text);
}

static void draw_box(int x, int y, int w, int h, unsigned long pixel)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(XtDisplay(perf_area), perf_gc, pixel);
    XDrawRectangle(XtDisplay(perf_area), XtWindow(perf_area), perf_gc, x, y,
                   (unsigned)w, (unsigned)h);
    XSetForeground(XtDisplay(perf_area), perf_gc, color_text);
}

static void draw_segment(int x0, int y0, int x1, int y1, unsigned long pixel)
{
    XSetForeground(XtDisplay(perf_area), perf_gc, pixel);
    XDrawLine(XtDisplay(perf_area), XtWindow(perf_area), perf_gc, x0, y0, x1, y1);
    XSetForeground(XtDisplay(perf_area), perf_gc, color_text);
}

struct perf_plot_rect {
    int x, y, w, h;
};

/* Draw one history ring as a polyline; TASKMGR_PERF_MISSING samples leave
 * gaps exactly like the windowd graphs did. */
static void draw_history_line(const struct perf_plot_rect *plot,
                              const uint8_t *history_values, unsigned long color)
{
    uint32_t count = history.count;
    int previous_set = 0;
    int previous_x = 0, previous_y = 0;
    if (!count) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t history_index =
            (history.head + TASKMGR_PERF_HISTORY - count + i) % TASKMGR_PERF_HISTORY;
        uint32_t slot = TASKMGR_PERF_HISTORY - count + i;
        int px, py;
        if (history_values[history_index] == TASKMGR_PERF_MISSING) {
            previous_set = 0;
            continue;
        }
        {
            uint32_t value = history_values[history_index] > 100U
                                 ? 100U : history_values[history_index];
            px = plot->x + 1 + (int)((slot * (uint32_t)(plot->w - 3)) /
                                     (TASKMGR_PERF_HISTORY - 1U));
            py = plot->y + plot->h - 2 -
                 (int)((value * (uint32_t)(plot->h - 3)) / 100U);
        }
        if (previous_set) {
            draw_segment(previous_x, previous_y, px, py, color);
        }
        previous_x = px;
        previous_y = py;
        previous_set = 1;
    }
}

static void draw_graph_frame(int x, int y, int w, int h, const char *title,
                             const char *current, int available,
                             struct perf_plot_rect *plot)
{
    unsigned long text_color = available ? color_text : color_muted;
    XSetForeground(XtDisplay(perf_area), perf_gc, text_color);
    draw_text(x + 8, y + 16, title);
    if (current) {
        draw_text(x + w - 60, y + 16, current);
    }
    XSetForeground(XtDisplay(perf_area), perf_gc, color_text);
    plot->x = x + 8;
    plot->y = y + 26;
    plot->w = w > 16 ? w - 16 : 1;
    plot->h = h > 34 ? h - 34 : 1;
    draw_fill(plot->x, plot->y, plot->w, plot->h, color_white);
    draw_box(plot->x, plot->y, plot->w, plot->h, color_border);
    if (plot->w < 4 || plot->h < 4) return;
    for (uint32_t step = 1U; step < 4U; ++step) {
        int gy = plot->y + ((plot->h - 1) * (int)step) / 4;
        draw_segment(plot->x + 1, gy, plot->x + plot->w - 2, gy, color_border);
    }
    if (plot->h >= 40) {
        XSetForeground(XtDisplay(perf_area), perf_gc, color_muted);
        draw_text(plot->x + 3, plot->y + 14, "100%");
        draw_text(plot->x + 3, plot->y + plot->h - 4, "0%");
        XSetForeground(XtDisplay(perf_area), perf_gc, color_text);
    }
}

static void draw_graph_core(int x, int y, int w, int h, const char *title,
                            const char *current, uint32_t core_count)
{
    struct perf_plot_rect plot;
    if (w < 96 || h < 32) return;
    draw_graph_frame(x, y, w, h, title, current, 1, &plot);
    if (plot.w < 4 || plot.h < 4 || !history.count) return;
    if (core_count > RELIEFOS_PERF_MAX_CPUS) core_count = RELIEFOS_PERF_MAX_CPUS;
    /* Core 0 first so later lines overdraw earlier ones; each core keeps
     * its stable palette color regardless of the draw order. */
    for (uint32_t core = 0; core < core_count; ++core) {
        draw_history_line(&plot, history.core[core], core_color(core));
    }
}

static void draw_graph(int x, int y, int w, int h, const char *title,
                       const char *current, const uint8_t *series,
                       unsigned long color)
{
    struct perf_plot_rect plot;
    if (w < 96 || h < 32) return;
    draw_graph_frame(x, y, w, h, title, current, series != 0, &plot);
    if (!series || plot.w < 4 || plot.h < 4) return;
    draw_history_line(&plot, series, color);
}

static void draw_text_line(int x, int y, const char *label, const char *value,
                           int area_h)
{
    if (y > area_h - 4) return;
    draw_text(x, y, label);
    draw_text(x + 150, y, value);
}

static void draw_performance(Widget widget)
{
    Dimension width = 0, height = 0;
    char value[64], value2[64], core_label[24];
    int content_w, content_bottom, graph_w, graph_h, graph_gap = 8;
    int graph_columns, y;
    uint32_t cpu_count = perf_info.cpu_count;
    uint64_t used_kib;

    if (!perf_gc) return;
    XtVaGetValues(widget, XmNwidth, &width, XmNheight, &height, NULL);
    content_w = (int)width > 24 ? (int)width - 24 : 1;
    content_bottom = (int)height - 8;
    if (!perf_valid) {
        XSetForeground(XtDisplay(widget), perf_gc, color_text);
        draw_text(24, 32, T("Performance data unavailable"));
        return;
    }

    used_kib = perf_info.total_memory_kib >= perf_info.free_memory_kib
                   ? perf_info.total_memory_kib - perf_info.free_memory_kib : 0;
    graph_columns = content_w >= 600 ? 3 : 1;
    graph_w = (content_w - graph_gap * (graph_columns - 1)) / graph_columns;
    graph_h = graph_columns == 1 ? 90 : ((int)height > 420 ? 120 : 96);
    if (graph_h > content_bottom - 8) graph_h = content_bottom - 8;
    y = 8;
    for (uint32_t i = 0; i < 3U; ++i) {
        int x = 8 + (int)i % graph_columns * (graph_w + graph_gap);
        int graph_y = y + (int)i / graph_columns * (graph_h + graph_gap);
        char current[24];
        if (i == 0U) {
            taskmgr_format_percent(current, sizeof(current), cpu_percent);
            draw_graph_core(x, graph_y, graph_w, graph_h, T("CPU Usage"),
                            current, perf_info.cpu_count);
        } else if (i == 1U) {
            taskmgr_format_percent(current, sizeof(current), mem_percent);
            draw_graph(x, graph_y, graph_w, graph_h, T("Memory Usage"),
                       current, history.memory, color_text);
        } else {
            if (gpu_sample.valid) {
                taskmgr_format_percent(current, sizeof(current), gpu_sample.percent);
            } else {
                snprintf(current, sizeof(current), "%s", "N/A");
            }
            draw_graph(x, graph_y, graph_w, graph_h,
                       graph_w < 210 ? T("GPU (est.)") : T("GPU (estimated)"),
                       current, gpu_sample.available ? history.gpu : 0,
                       color_accent);
        }
    }
    y += graph_h + graph_gap + 18;
    if (cpu_count > RELIEFOS_PERF_MAX_CPUS) cpu_count = RELIEFOS_PERF_MAX_CPUS;
    XSetForeground(XtDisplay(widget), perf_gc, color_text);
    draw_text(24, y, T("Per-core usage"));
    y += 22;
    {
        int columns = cpu_count > 16U ? 3 : (cpu_count > 8U ? 2 : 1);
        if (cpu_count == 1U) columns = 1;
        if (columns > content_w / 160 && content_w / 160 > 0) {
            columns = content_w / 160;
        }
        if (content_w < 200) columns = 1;
        int column_w = columns ? content_w / columns : content_w;
        for (uint32_t i = 0; i < cpu_count; ++i) {
            int column = (int)i % columns;
            int row = (int)i / columns;
            int x = 24 + column * column_w;
            int row_y = y + row * 28;
            int progress_x = x + 48;
            int progress_w = column_w > 104 ? column_w - 104 : 24;
            if (row_y + 16 > content_bottom) break;
            snprintf(core_label, sizeof(core_label), "CPU %u", i);
            draw_fill(x, row_y + 2, 6, 8, core_color(i));
            XSetForeground(XtDisplay(widget), perf_gc, color_text);
            draw_text(x + 10, row_y + 10, core_label);
            draw_box(progress_x, row_y, progress_w, 16, color_border);
            draw_fill(progress_x + 1, row_y + 1,
                      (progress_w - 2) * (int)cpu_percent_by_core[i] / 100, 14,
                      core_color(i));
            taskmgr_format_percent(value, sizeof(value), cpu_percent_by_core[i]);
            XSetForeground(XtDisplay(widget), perf_gc, color_text);
            draw_text(x + column_w - 44, row_y + 10, value);
        }
        y += ((int)((cpu_count + columns - 1) / (columns ? columns : 1))) * 28 + 6;
    }

    taskmgr_format_kib(value, sizeof(value), perf_info.total_memory_kib);
    draw_text_line(24, y, T("Total memory:"), value, content_bottom);
    y += 18;
    taskmgr_format_kib(value, sizeof(value), used_kib);
    draw_text_line(24, y, T("Used memory:"), value, content_bottom);
    y += 18;
    taskmgr_format_kib(value, sizeof(value), perf_info.free_memory_kib);
    draw_text_line(24, y, T("Free memory:"), value, content_bottom);
    y += 18;
    taskmgr_format_uptime(value, sizeof(value), perf_info.uptime_ms);
    draw_text_line(24, y, T("Uptime:"), value, content_bottom);
    y += 24;
    snprintf(value, sizeof(value), "%s%u / %s%u", T("Tasks "),
             perf_info.task_count, T("Run "), perf_info.running_tasks);
    snprintf(value2, sizeof(value2), "%s%u / %s%u", T("Ready "),
             perf_info.ready_tasks, T("Sleep "), perf_info.sleeping_tasks);
    draw_text_line(24, y, value, value2, content_bottom);
}

static void perf_expose(Widget widget, XtPointer data, XtPointer call)
{
    (void)data;
    (void)call;
    draw_performance(widget);
}

static void perf_resize(Widget widget, XtPointer data, XtPointer call)
{
    (void)data;
    (void)call;
    draw_performance(widget);
}

static void kill_selected_task(void)
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
    if (reliefos_task_kill(task->pid) < 0) {
        set_status(T("End Task failed"));
        return;
    }
    set_status(T("Task ended"));
    refresh_all();
}

static int dialog_answer;

static void dialog_reply(Widget widget, XtPointer answer, XtPointer call)
{
    (void)widget;
    (void)call;
    dialog_answer = (int)(intptr_t)answer;
}

static int dialog_wait(Widget dialog)
{
    dialog_answer = 0;
    XtVaSetValues(dialog, XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL,
                  XmNautoUnmanage, False, NULL);
    XtVaSetValues(XtParent(dialog), XmNdeleteResponse, XmDO_NOTHING, NULL);
    Atom close = XInternAtom(XtDisplay(dialog), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(XtParent(dialog), close, dialog_reply, (XtPointer)-1);
    XtManageChild(dialog);
    while (!dialog_answer && !XtAppGetExitFlag(app)) {
        XtAppProcessEvent(app, XtIMAll);
    }
    XtUnmanageChild(dialog);
    return dialog_answer == 1;
}

static int confirm_dialog(const char *title, const char *message)
{
    Widget dialog = XmCreateQuestionDialog(shell, "confirmation", NULL, 0);
    XmString caption = XmStringCreateLocalized((char *)title);
    XmString body = XmStringCreateLocalized((char *)message);
    XtVaSetValues(dialog, XmNdialogTitle, caption, XmNmessageString, body,
                  XmNdefaultButtonType, XmDIALOG_CANCEL_BUTTON, NULL);
    XmStringFree(caption);
    XmStringFree(body);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtAddCallback(dialog, XmNokCallback, dialog_reply, (XtPointer)1);
    XtAddCallback(dialog, XmNcancelCallback, dialog_reply, (XtPointer)-1);
    int accepted = dialog_wait(dialog);
    XtDestroyWidget(XtParent(dialog));
    return accepted;
}

static void show_task_details(void)
{
    struct reliefos_task_info *task = selected_task();
    struct reliefos_task_info snapshot;
    char body[768];
    char pid[16], ppid[16], cr3[24], entry[24], wake[16], ticks[32], mem[24];
    Widget dialog;
    XmString caption, message;

    if (!task) {
        set_status(T("No task selected"));
        return;
    }
    snapshot = *task;
    taskmgr_format_dec(pid, sizeof(pid), snapshot.pid);
    taskmgr_format_dec(ppid, sizeof(ppid), snapshot.parent_pid);
    taskmgr_format_hex_fixed(cr3, sizeof(cr3), snapshot.cr3, 12);
    taskmgr_format_hex_fixed(entry, sizeof(entry), snapshot.entry, 12);
    taskmgr_format_dec(wake, sizeof(wake), snapshot.wake_tick);
    {
        char tick_count[24];
        taskmgr_format_dec(tick_count, sizeof(tick_count), snapshot.cpu_ticks);
        snprintf(ticks, sizeof(ticks), "%s %s", tick_count, T("ticks"));
    }
    taskmgr_format_memory(mem, sizeof(mem), snapshot.memory_kib);
    snprintf(body, sizeof(body),
             "%s %s\nPID: %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n%s %s\n"
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
             cr3,
             entry);

    dialog = XmCreateInformationDialog(shell, "taskDetails", NULL, 0);
    caption = XmStringCreateLocalized((char *)T("Task Details"));
    message = XmStringCreateLocalized(body);
    XtVaSetValues(dialog, XmNdialogTitle, caption, XmNmessageString, message, NULL);
    XmStringFree(caption);
    XmStringFree(message);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
    XtAddCallback(dialog, XmNokCallback, dialog_reply, (XtPointer)1);
    dialog_wait(dialog);
    XtDestroyWidget(XtParent(dialog));
}

static void toggle_selected_startup_entry(void)
{
    struct reliefos_startup_entry *entry = (selected_startup >= 0 &&
                                            (uint32_t)selected_startup < startup_entry_count)
                                               ? &startup_entries[selected_startup]
                                               : 0;
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

static void remove_selected_startup_entry(void)
{
    struct reliefos_startup_entry *entry = (selected_startup >= 0 &&
                                            (uint32_t)selected_startup < startup_entry_count)
                                               ? &startup_entries[selected_startup]
                                               : 0;
    if (!entry) {
        set_status(T("No startup app selected"));
        return;
    }
    if (!confirm_dialog(T("Remove Startup App"), T("Remove the selected startup app?"))) {
        return;
    }
    if (reliefos_startup_remove(startup_selected_uid, entry->id) < 0) {
        set_status(T("Could not remove startup app"));
        return;
    }
    set_status(T("Startup app removed"));
    refresh_startup_entries();
    update_controls();
}

static void show_about(void)
{
    Widget dialog = XmCreateInformationDialog(shell, "about", NULL, 0);
    XmString caption = XmStringCreateLocalized((char *)T("Task Manager"));
    XmString message = XmStringCreateLocalized(
        (char *)T("Live task snapshot from the scheduler."));
    XtVaSetValues(dialog, XmNdialogTitle, caption, XmNmessageString, message, NULL);
    XmStringFree(caption);
    XmStringFree(message);
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
    XtAddCallback(dialog, XmNokCallback, dialog_reply, (XtPointer)1);
    dialog_wait(dialog);
    XtDestroyWidget(XtParent(dialog));
}

static void select_tab(Widget page)
{
    if (!page) return;
    XmTabStackSelectTab(page, True);
}

static void menu_callback(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)call;
    switch ((int)(intptr_t)data) {
    case 1: refresh_all(); break;
    case 2: kill_selected_task(); break;
    case 3: show_task_details(); break;
    case 4: show_about(); break;
    case 5: select_tab(page_processes); break;
    case 6: select_tab(page_performance); break;
    case 7: select_tab(page_startup); break;
    case 8: toggle_selected_startup_entry(); break;
    case 9: remove_selected_startup_entry(); break;
    default: break;
    }
}

static void tab_selected(Widget widget, XtPointer data, XtPointer call)
{
    XmTabStackCallbackStruct *selection = call;
    (void)widget;
    (void)data;
    if (!selection || !selection->selected_child) return;
    if (selection->selected_child == page_processes) active_tab = TASKMGR_TAB_PROCESSES;
    else if (selection->selected_child == page_performance) active_tab = TASKMGR_TAB_PERFORMANCE;
    else if (selection->selected_child == page_startup) active_tab = TASKMGR_TAB_STARTUP;
    if (active_tab == TASKMGR_TAB_STARTUP) refresh_startup();
    update_controls();
}

static void select_process(Widget widget, XtPointer data, XtPointer call)
{
    XmListCallbackStruct *selection = call;
    (void)widget;
    (void)data;
    if (updating || selection->item_position < 1 ||
        (uint32_t)selection->item_position > tree_count) return;
    selected_pid = tasks[tree_rows[selection->item_position - 1].index].pid;
    update_controls();
}

static void select_startup(Widget widget, XtPointer data, XtPointer call)
{
    XmListCallbackStruct *selection = call;
    (void)widget;
    (void)data;
    if (updating || selection->item_position < 1 ||
        (uint32_t)selection->item_position > startup_entry_count) return;
    selected_startup = selection->item_position - 1;
    update_controls();
}

static void select_user(Widget widget, XtPointer data, XtPointer call)
{
    XmComboBoxCallbackStruct *selection = call;
    char *name = 0;
    (void)widget;
    (void)data;
    if (updating) return;
    /* Match the chosen name instead of trusting the combo box position
     * convention: the item list is exactly the user list either way. */
    if (selection->item_or_text &&
        XmStringGetLtoR(selection->item_or_text, XmFONTLIST_DEFAULT_TAG, &name)) {
        for (uint32_t i = 0; i < startup_user_count; ++i) {
            if (!strcmp(startup_users[i].username, name)) {
                startup_selected_uid = startup_users[i].uid;
                break;
            }
        }
        XtFree(name);
    }
    selected_startup = -1;
    refresh_startup_entries();
    update_controls();
}

static void popup_details_action(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    show_task_details();
}

static void right_click(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    (void)data;
    (void)dispatch;
    if (event->type != ButtonPress || event->xbutton.button != Button3) return;
    if (widget != process_list) return;
    int position = XmListYToPos(process_list, event->xbutton.y);
    if (position > 0 && (uint32_t)position <= tree_count) {
        XmListDeselectAllItems(process_list);
        XmListSelectPos(process_list, position, False);
        selected_pid = tasks[tree_rows[position - 1].index].pid;
    } else {
        selected_pid = 0;
    }
    update_controls();
    XmMenuPosition(process_popup, &event->xbutton);
    XtManageChild(process_popup);
}

static void key(Widget widget, XtPointer data, XEvent *event, Boolean *dispatch)
{
    KeySym symbol;
    (void)data;
    if (event->type != KeyPress) return;
    symbol = XLookupKeysym(&event->xkey, 0);
    if (symbol == XK_F5) {
        *dispatch = False;
        refresh_all();
        return;
    }
    if (symbol != XK_Delete) return;
    *dispatch = False;
    if (widget == process_list) kill_selected_task();
    else if (widget == startup_list_widget) remove_selected_startup_entry();
}

static void close_window(Widget widget, XtPointer data, XtPointer call)
{
    (void)widget;
    (void)data;
    (void)call;
    XtAppSetExitFlag(app);
}

static Widget action_button(Widget parent, const char *label, int action)
{
    Widget button = XtVaCreateManagedWidget(label, xmPushButtonWidgetClass, parent, NULL);
    set_label(button, T(label));
    XtAddCallback(button, XmNactivateCallback, menu_callback, (XtPointer)(intptr_t)action);
    return button;
}

static Widget menu(Widget bar, const char *name)
{
    Widget pane = XmCreatePulldownMenu(bar, (char *)name, NULL, 0);
    Widget cascade = XtVaCreateManagedWidget(name, xmCascadeButtonWidgetClass, bar,
                                             XmNsubMenuId, pane, NULL);
    set_label(cascade, T(name));
    return pane;
}

static void tick(XtPointer data, XtIntervalId *id)
{
    (void)data;
    (void)id;
    refresh_all();
    XtAppAddTimeOut(app, 500, tick, NULL);
}

static Widget tab_page(const char *name, const char *title)
{
    XmString label = XmStringCreateLocalized((char *)title);
    Widget page = XtVaCreateManagedWidget(name, xmFormWidgetClass, tab_stack,
                                          XmNtabLabelString, label, NULL);
    XmStringFree(label);
    return page;
}

static void build_processes_page(void)
{
    Widget heading = XtVaCreateManagedWidget("processColumns", xmLabelWidgetClass,
                                             page_processes,
                                             XmNtopAttachment, XmATTACH_FORM,
                                             XmNleftAttachment, XmATTACH_FORM,
                                             XmNrightAttachment, XmATTACH_FORM,
                                             XmNalignment, XmALIGNMENT_BEGINNING,
                                             NULL);
    char columns[160];
    snprintf(columns, sizeof(columns),
             "%-16s %7s %6s %9s %-5s %-10s %s",
             T("PROCESS"), "PID", T("CPU"), T("MEM"), T("STATE"), T("USER"),
             T("PRIV"));
    set_label(heading, columns);
    process_list = XmCreateScrolledList(page_processes, "processes", NULL, 0);
    XtVaSetValues(process_list, XmNselectionPolicy, XmBROWSE_SELECT,
                  XmNvisibleItemCount, 16, NULL);
    XtVaSetValues(XtParent(process_list),
                  XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, heading,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(process_list, XmNbrowseSelectionCallback, select_process, NULL);
    XtAddCallback(process_list, XmNdefaultActionCallback, popup_details_action, NULL);
    XtInsertEventHandler(process_list, KeyPressMask, False, key, NULL, XtListHead);
    XtAddEventHandler(process_list, ButtonPressMask, False, right_click, NULL);
    XtManageChild(process_list);
    process_popup = XmCreatePopupMenu(process_list, "processContext", NULL, 0);
    popup_end_task = action_button(process_popup, "End Task", 2);
    popup_details = action_button(process_popup, "Details", 3);
    action_button(process_popup, "Refresh", 1);
}

static void build_performance_page(void)
{
    perf_area = XtVaCreateManagedWidget("performance", xmDrawingAreaWidgetClass,
                                        page_performance,
                                        XmNtopAttachment, XmATTACH_FORM,
                                        XmNbottomAttachment, XmATTACH_FORM,
                                        XmNleftAttachment, XmATTACH_FORM,
                                        XmNrightAttachment, XmATTACH_FORM,
                                        NULL);
    XtAddCallback(perf_area, XmNexposeCallback, perf_expose, NULL);
    XtAddCallback(perf_area, XmNresizeCallback, perf_resize, NULL);
}

static void build_startup_page(void)
{
    startup_user_label = XtVaCreateManagedWidget("userLabel", xmLabelWidgetClass,
                                                 page_startup,
                                                 XmNtopAttachment, XmATTACH_FORM,
                                                 XmNleftAttachment, XmATTACH_FORM,
                                                 NULL);
    set_label(startup_user_label, T("User"));
    startup_combo = XmCreateDropDownComboBox(page_startup, "userCombo", NULL, 0);
    XtVaSetValues(startup_combo,
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, startup_user_label,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNvisibleItemCount, 6, NULL);
    XtAddCallback(startup_combo, XmNselectionCallback, select_user, NULL);
    XtManageChild(startup_combo);
    Widget toolbar = XtVaCreateManagedWidget("startupToolbar", xmRowColumnWidgetClass,
                                             page_startup,
                                             XmNorientation, XmHORIZONTAL,
                                             XmNpacking, XmPACK_TIGHT,
                                             XmNtopAttachment, XmATTACH_WIDGET,
                                             XmNtopWidget, startup_combo,
                                             XmNleftAttachment, XmATTACH_FORM,
                                             XmNrightAttachment, XmATTACH_FORM, NULL);
    button_startup_toggle = action_button(toolbar, "Enable", 8);
    button_startup_remove = action_button(toolbar, "Remove", 9);
    startup_list_widget = XmCreateScrolledList(page_startup, "startupEntries", NULL, 0);
    XtVaSetValues(startup_list_widget, XmNselectionPolicy, XmBROWSE_SELECT,
                  XmNvisibleItemCount, 10, NULL);
    XtVaSetValues(XtParent(startup_list_widget),
                  XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, toolbar,
                  XmNbottomAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM, NULL);
    XtAddCallback(startup_list_widget, XmNbrowseSelectionCallback, select_startup, NULL);
    XtInsertEventHandler(startup_list_widget, KeyPressMask, False, key, NULL, XtListHead);
    XtManageChild(startup_list_widget);
}

static void build_ui(void)
{
    Widget form = XtVaCreateWidget("taskManager", xmFormWidgetClass, shell,
                                   XmNwidth, 720, XmNheight, 560,
                                   XmNresizePolicy, XmRESIZE_NONE,
                                   XmNmarginWidth, 8, XmNmarginHeight, 8, NULL);
    Widget bar = XmCreateMenuBar(form, "menuBar", NULL, 0);
    XtVaSetValues(bar, XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM, NULL);
    Widget file_menu = menu(bar, "File");
    action_button(file_menu, "Refresh", 1);
    menu_end_task = action_button(file_menu, "End Task", 2);
    action_button(file_menu, "About", 4);
    Widget options_menu = menu(bar, "Options");
    action_button(options_menu, "Processes", 5);
    action_button(options_menu, "Performance", 6);
    action_button(options_menu, "Service Manager", 7);
    action_button(options_menu, "About", 4);
    XtManageChild(bar);

    Widget toolbar = XtVaCreateManagedWidget("toolbar", xmRowColumnWidgetClass, form,
                                             XmNorientation, XmHORIZONTAL,
                                             XmNpacking, XmPACK_TIGHT,
                                             XmNtopAttachment, XmATTACH_WIDGET,
                                             XmNtopWidget, bar,
                                             XmNleftAttachment, XmATTACH_FORM,
                                             XmNrightAttachment, XmATTACH_FORM, NULL);
    action_button(toolbar, "Refresh", 1);
    button_end_task = action_button(toolbar, "End Task", 2);
    action_button(toolbar, "Details", 3);

    status_label = XtVaCreateManagedWidget("status", xmLabelWidgetClass, form,
                                           XmNalignment, XmALIGNMENT_BEGINNING,
                                           XmNrecomputeSize, False, XmNheight, 26,
                                           XmNbottomAttachment, XmATTACH_FORM,
                                           XmNleftAttachment, XmATTACH_FORM,
                                           XmNrightAttachment, XmATTACH_FORM, NULL);
    set_status("Ready");

    tab_stack = XmCreateTabStack(form, "tabs", NULL, 0);
    XtVaSetValues(tab_stack,
                  XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, toolbar,
                  XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, status_label,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM, NULL);
    page_processes = tab_page("processesTab", T("Processes"));
    page_performance = tab_page("performanceTab", T("Performance"));
    page_startup = tab_page("startupTab", T("Service Manager"));
    XtAddCallback(tab_stack, XmNtabSelectedCallback, tab_selected, NULL);
    XtManageChild(tab_stack);

    build_processes_page();
    build_performance_page();
    build_startup_page();

    XtManageChild(form);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("leonos", RELIEFOS_LAYOUT_LOCALE);
    textdomain("leonos");
    XtSetLanguageProc(NULL, NULL, NULL);
    char *fallback[] = {
        "*fontList: fixed", "*background: #eceef4", "*foreground: #22242e",
        "*highlightColor: #3b62a6", NULL
    };
    shell = XtVaAppInitialize(&app, "ReliefOSTaskManager", NULL, 0,
                              &argc, argv, fallback,
                              XtNtitle, T("Task Manager"),
                              XtNwidth, 720, XtNheight, 560, NULL);
    taskmgr_perf_history_init(&history);
    build_ui();
    XtRealizeWidget(shell);
    {
        Display *display = XtDisplay(shell);
        perf_gc = XCreateGC(display, XtWindow(shell), 0, NULL);
        perf_font = XLoadFont(display, "fixed");
        if (perf_font) XSetFont(display, perf_gc, perf_font);
        for (uint32_t i = 0; i < TASKMGR_CORE_PALETTE_SIZE; ++i) {
            perf_pixels[i] = alloc_color(display, core_palette[i]);
        }
        color_white = alloc_color(display, 0x00FFFFFF);
        color_border = alloc_color(display, 0x00B0B4C0);
        color_muted = alloc_color(display, 0x00808694);
        color_text = alloc_color(display, 0x0022242E);
        color_accent = alloc_color(display, 0x003B62A6);
        XSetForeground(display, perf_gc, color_text);
        XSetBackground(display, perf_gc,
                       WhitePixel(display, DefaultScreen(display)));
    }
    Atom delete_window = XInternAtom(XtDisplay(shell), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(shell, delete_window, close_window, NULL);
    refresh_performance();
    refresh_tasks();
    refresh_startup();
    update_controls();
    puts("[taskmgr.elf] Motif task manager ready");
    fflush(stdout);
    XtAppAddTimeOut(app, 500, tick, NULL);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);
    XtDestroyApplicationContext(app);
    return 0;
}
