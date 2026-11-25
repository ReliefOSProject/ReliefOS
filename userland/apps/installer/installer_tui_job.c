#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "installer_tui.h"
#include "installer_job.h"
#include "install_ops.h"

#define T(s) gettext(s)

/* The ncurses loop never opens or reads the log filesystem. A small disposable
 * reader sends one tail snapshot through a nonblocking pipe once per second. */
static struct {
    pid_t pid;
    int fd, eof, stopped;
    size_t used;
    uint64_t started, refreshed;
    char incoming[8192], text[8192];
} log_reader = {.fd = -1};

static uint64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void log_reader_stop(void)
{
    if (log_reader.fd >= 0) close(log_reader.fd);
    log_reader.fd = -1;
    log_reader.eof = log_reader.stopped = 1;
    if (log_reader.pid > 0) {
        (void)kill(log_reader.pid, SIGKILL);
        /* A stuck kernel read must never block the UI during cleanup. A
         * surviving reader is reaped by the next pump (or PID 1 on exit). */
        pid_t result = waitpid(log_reader.pid, NULL, WNOHANG);
        if (result == log_reader.pid || (result < 0 && errno == ECHILD)) log_reader.pid = 0;
    }
}

static void log_reader_step(void)
{
    if (log_reader.pid <= 0) return;
    while (log_reader.fd >= 0 && !log_reader.eof && log_reader.used < sizeof(log_reader.incoming) - 1) {
        ssize_t count = read(log_reader.fd, log_reader.incoming + log_reader.used,
            sizeof(log_reader.incoming) - 1 - log_reader.used);
        if (count > 0) { log_reader.used += (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (!count || (errno != EAGAIN && errno != EWOULDBLOCK)) log_reader.eof = 1;
        break;
    }
    if (log_reader.used == sizeof(log_reader.incoming) - 1) log_reader.eof = 1;
    int status = 0;
    pid_t result = waitpid(log_reader.pid, &status, WNOHANG);
    if (result == log_reader.pid || (result < 0 && errno == ECHILD)) {
        if (!log_reader.stopped && result > 0 && WIFEXITED(status) && !WEXITSTATUS(status)) {
            log_reader.incoming[log_reader.used] = 0;
            memcpy(log_reader.text, log_reader.incoming, log_reader.used + 1);
        }
        if (log_reader.fd >= 0) close(log_reader.fd);
        log_reader.fd = -1;
        log_reader.pid = 0;
    } else {
        uint64_t now = now_ms();
        if (now < log_reader.started || now - log_reader.started >= 1000) log_reader_stop();
    }
}

static void log_reader_start(const char *path)
{
    uint64_t now = now_ms();
    if (log_reader.pid > 0 || (log_reader.refreshed && now >= log_reader.refreshed &&
        now - log_reader.refreshed < 1000)) return;
    log_reader.refreshed = now;
    int descriptors[2];
    if (pipe(descriptors) < 0) return;
    if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(descriptors[0]); close(descriptors[1]); return;
    }
    pid_t child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); return; }
    if (!child) {
        close(descriptors[0]);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        char text[8192];
        tui_read_log(path, text, sizeof(text));
        size_t used = 0, length = strlen(text);
        while (used < length) {
            ssize_t count = write(descriptors[1], text + used, length - used);
            if (count > 0) used += (size_t)count;
            else if (count < 0 && errno == EINTR) continue;
            else break;
        }
        close(descriptors[1]);
        _exit(used == length ? 0 : 1);
    }
    close(descriptors[1]);
    log_reader.pid = child;
    log_reader.fd = descriptors[0];
    log_reader.eof = log_reader.stopped = 0;
    log_reader.used = 0;
    log_reader.started = now;
}

static int job_error(int error)
{
    copy_text(status_text, sizeof(status_text), T("Operation failed"));
    snprintf(detail_text, sizeof(detail_text), "%s (%d)", strerror(-error), error);
    return error;
}

void installer_tui_publish_progress(void)
{
    struct installer_job_packet packet = {0};
    packet.progress = progress_value;
    snprintf(packet.message, sizeof(packet.message), "%s", status_text);
    snprintf(packet.detail, sizeof(packet.detail), "%s", detail_text);
    installer_job_publish(&packet);
}

static void update_progress(void *context, unsigned value, const char *message)
{
    struct installer_update_info *info = context;
    struct installer_job_packet packet = {0};
    packet.progress = value;
    snprintf(packet.message, sizeof(packet.message), "%s", T(message));
    installer_update_activity_format(&info->activity, packet.detail, sizeof(packet.detail));
    progress_value = value;
    copy_text(status_text, sizeof(status_text), packet.message);
    copy_text(detail_text, sizeof(detail_text), packet.detail);
    installer_job_publish(&packet);
}

static int execute(const struct installer_tty_context *context, struct tui_job *job,
                   struct installer_update_info *result)
{
    switch (job->kind) {
    case TUI_JOB_INSTALL:
        return context->perform_install();
    case TUI_JOB_CHECK:
        return installer_update_check_target(context->disks[*context->selected_disk].path,
            job->root_uuid, job->esp_uuid, result, update_progress, result);
    case TUI_JOB_UPDATE:
        return installer_update_apply(&job->info,
            (context->setup->skip_bios ? 0 : 1u) | (context->setup->skip_uefi ? 0 : 2u),
            result, update_progress, result);
    default:
        /* Draft partition edits belong to the UI process. A worker would
         * discard their memory when it exits, so refuse that misuse. */
        return -EINVAL;
    }
}

struct run_context {
    const struct installer_tty_context *context;
    struct tui_job *job;
};

static int run_job(void *opaque, struct installer_job_packet *packet)
{
    struct run_context *run = opaque;
    const struct installer_tty_context *context = run->context;
    struct tui_job *job = run->job;
    int result = execute(context, job, &packet->info);
    if (!result && job->kind == TUI_JOB_INSTALL && context->boot_entry_path)
        context->boot_entry_path(packet->info.boot_file, sizeof(packet->info.boot_file));
    packet->progress = progress_value;
    packet->selected = *context->selected_partition;
    packet->root = *context->root_partition;
    packet->esp = *context->esp_partition;
    packet->automatic = *context->partition_auto;
    packet->replaced = installer_partition_table_replaced;
    packet->format_esp = context->setup->format_esp;
    packet->success = *context->install_success;
    snprintf(packet->message, sizeof(packet->message), "%s", status_text);
    snprintf(packet->detail, sizeof(packet->detail), "%s", detail_text);
    return result;
}

int tui_run_job(const struct installer_tty_context *context, struct tui_job *job)
{
    struct installer_job state;
    struct run_context run = {context, job};
    const char *log_path = job->kind == TUI_JOB_CHECK || job->kind == TUI_JOB_UPDATE
        ? "/var/log/installer-update.log" : "/var/log/installer.log";
    /* tui_open() already directs stdout and stderr to the installer log.
     * The update engine owns its separate log and reports any open error. */
    int result = installer_job_start(&state, run_job, &run, NULL);
    if (result) return job_error(result);
    struct tui_view view = {.previous_focus = -1};
    int show_log = 0, cancel_requested = 0;
    uint64_t stop_sent = 0;
    log_reader_step();
    log_reader.text[0] = 0;
    log_reader.refreshed = 0;
    while ((result = installer_job_step(&state)) > 0) {
        log_reader_step();
        if (cancel_requested && now_ms() - stop_sent >= 500) {
            (void)installer_job_request_stop(&state);
            stop_sent = now_ms();
        }
        const struct installer_job_packet *packet = &state.latest;
        unsigned progress = packet->progress > 100 ? 100 : packet->progress;
        tui_begin(&view, job->kind == TUI_JOB_CHECK ? T("Checking installed system")
            : installer_model_page_title(INSTALLER_PAGE_PROGRESS, *context->install_mode));
        char bar[96], fill[51];
        memset(fill, ' ', 50);
        memset(fill, '#', progress / 2);
        fill[50] = 0;
        snprintf(bar, sizeof(bar), "[%s] %3u%%", fill, progress);
        tui_text(&view, bar);
        tui_text(&view, *packet->message ? packet->message : T("Preparing operation"));
        tui_text(&view, packet->detail);
        tui_text(&view, T("Disk operations are running. Please wait for completion."));
        tui_text(&view, log_path);
        tui_button(&view, T("Show / hide log (F4)"), TUI_LOG);
        if (show_log) { log_reader_start(log_path); tui_document(&view, log_reader.text); }
        if (cancel_requested) snprintf(view.status, sizeof(view.status), "%s", T("Canceling the update check. Waiting for cleanup."));
        else if (installer_tui_signal) snprintf(view.status, sizeof(view.status), "%s", T("The terminal requested exit. Waiting for the disk operation to finish."));
        int action = tui_step(&view, NULL, 0, job->kind == TUI_JOB_CHECK && !cancel_requested, 100);
        if (action == TUI_LOG) show_log = !show_log;
        if (action == TUI_CANCEL && job->kind == TUI_JOB_CHECK && !cancel_requested &&
            !installer_job_request_stop(&state)) {
            cancel_requested = 1;
            stop_sent = now_ms();
        }
    }
    log_reader_stop();
    if (result < 0) return job_error(result);
    const struct installer_job_packet *final = &state.final;
    *context->selected_partition = final->selected;
    *context->root_partition = final->root;
    *context->esp_partition = final->esp;
    *context->partition_auto = final->automatic;
    installer_partition_table_replaced = final->replaced;
    context->setup->format_esp = final->format_esp;
    if (job->kind == TUI_JOB_INSTALL) *context->install_success = final->success;
    job->info = final->info;
    copy_text(status_text, sizeof(status_text), final->message);
    copy_text(detail_text, sizeof(detail_text), final->detail);
    if (final->result) {
        snprintf(detail_text, sizeof(detail_text), "%s (%d)", strerror(-final->result), final->result);
        size_t position = strlen(detail_text);
        if (position + 1 < sizeof(detail_text)) {
            detail_text[position++] = '\n';
            copy_text(detail_text + position, sizeof(detail_text) - position, final->detail);
        }
    }
    return final->result;
}
