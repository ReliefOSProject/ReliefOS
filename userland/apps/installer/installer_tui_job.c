#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <string.h>
#include "installer_tui.h"
#include "installer_job.h"
#include "install_ops.h"

#define T(s) gettext(s)

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
    case TUI_JOB_AUTO: return context->partition_auto_layout();
    case TUI_JOB_INIT: return context->partition_initialize();
    case TUI_JOB_CREATE: return context->partition_create(job->filesystem, job->size, job->name);
    case TUI_JOB_RESIZE: return context->partition_resize(job->size);
    case TUI_JOB_RENAME: return context->partition_rename(job->name);
    case TUI_JOB_FORMAT: return context->partition_format(job->filesystem, job->name);
    case TUI_JOB_DELETE: return context->partition_delete();
    case TUI_JOB_ROLE: return context->partition_set_type(job->role);
    }
    return -EINVAL;
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
    int show_log = 0;
    while ((result = installer_job_step(&state)) > 0) {
        const struct installer_job_packet *packet = &state.latest;
        unsigned progress = packet->progress > 100 ? 100 : packet->progress;
        tui_begin(&view, T("Operation in progress"));
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
        char log[8192];
        if (show_log) { tui_read_log(log_path, log, sizeof(log)); tui_document(&view, log); }
        if (installer_tui_signal) snprintf(view.status, sizeof(view.status), "%s", T("The terminal requested exit. Waiting for the disk operation to finish."));
        if (tui_step(&view, NULL, 0, 0, 100) == TUI_LOG) show_log = !show_log;
    }
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
