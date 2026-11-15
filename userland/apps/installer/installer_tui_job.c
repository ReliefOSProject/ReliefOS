#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "installer_tui.h"
#include "install_ops.h"

#define T(s) gettext(s)
struct tui_packet {
    unsigned complete, progress;
    int result, selected, root, esp;
    unsigned automatic, replaced, format_esp, success;
    char message[256], detail[768];
    struct installer_update_info info;
};
static int progress_fd = -1;

static int job_error(int error)
{
    copy_text(status_text, sizeof(status_text), T("Operation failed"));
    snprintf(detail_text, sizeof(detail_text), "%s (%d)", strerror(-error), error);
    return error;
}

static int send_packet(const struct tui_packet *packet)
{
    const unsigned char *data = (const unsigned char *)packet;
    size_t left = sizeof(*packet);
    while (left) {
        ssize_t count = write(progress_fd, data, left);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        data += count; left -= count;
    }
    return 0;
}

void installer_tui_publish_progress(void)
{
    if (progress_fd < 0) return;
    struct tui_packet packet = {0};
    packet.progress = progress_value;
    snprintf(packet.message, sizeof(packet.message), "%s", status_text);
    snprintf(packet.detail, sizeof(packet.detail), "%s", detail_text);
    send_packet(&packet);
}

static void update_progress(void *context, unsigned value, const char *message)
{
    struct installer_update_info *info = context;
    struct tui_packet packet = {0};
    packet.progress = value;
    snprintf(packet.message, sizeof(packet.message), "%s", T(message));
    installer_update_activity_format(&info->activity, packet.detail, sizeof(packet.detail));
    progress_value = value;
    copy_text(status_text, sizeof(status_text), packet.message);
    copy_text(detail_text, sizeof(detail_text), packet.detail);
    send_packet(&packet);
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

/* Workers inherit the confirmed in-memory plan. Only the parent owns curses;
 * EOF, partial packets and abnormal exits cannot become a successful result. */
int tui_run_job(const struct installer_tty_context *context, struct tui_job *job)
{
    int descriptors[2];
    if (pipe(descriptors) < 0) return job_error(-errno);
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0) {
        int saved = errno; close(descriptors[0]); close(descriptors[1]); return job_error(-saved);
    }
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { int saved = errno; close(descriptors[0]); close(descriptors[1]); return job_error(-saved); }
    if (!child) {
        close(descriptors[0]); progress_fd = descriptors[1];
        signal(SIGPIPE, SIG_IGN);
        /* Finish the authorized disk operation even if the terminal hangs up.
         * External programs receive /dev/null as stdin and cannot prompt. */
        signal(SIGHUP, SIG_IGN); signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
        int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (input >= 0) { dup2(input, STDIN_FILENO); close(input); }
        struct tui_packet packet = {0};
        packet.result = execute(context, job, &packet.info);
        if (!packet.result && job->kind == TUI_JOB_INSTALL && context->boot_entry_path)
            context->boot_entry_path(packet.info.boot_file, sizeof(packet.info.boot_file));
        packet.complete = 1;
        packet.progress = packet.result ? 0 : 100;
        packet.selected = *context->selected_partition;
        packet.root = *context->root_partition;
        packet.esp = *context->esp_partition;
        packet.automatic = *context->partition_auto;
        packet.replaced = installer_partition_table_replaced;
        packet.format_esp = context->setup->format_esp;
        packet.success = *context->install_success;
        snprintf(packet.message, sizeof(packet.message), "%s", status_text);
        snprintf(packet.detail, sizeof(packet.detail), "%s", detail_text);
        int error = send_packet(&packet);
        close(progress_fd); fflush(NULL);
        _exit(error ? 1 : 0);
    }
    close(descriptors[1]);
    struct tui_packet packet = {0}, final = {0};
    struct tui_view view = {.previous_focus = -1};
    size_t received = 0;
    int complete = 0, eof = 0, exited = 0, status = 0, read_error = 0;
    char message[256], detail[768] = "";
    snprintf(message, sizeof(message), "%s", T("Preparing operation"));
    unsigned progress = 0;
    int show_log = 0;
    const char *log_path = job->kind == TUI_JOB_CHECK || job->kind == TUI_JOB_UPDATE
        ? "/var/log/installer-update.log" : "/var/log/installer.log";
    while (!exited || !eof) {
        while (!eof) {
            ssize_t count = read(descriptors[0], (unsigned char *)&packet + received, sizeof(packet) - received);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count < 0) { read_error = errno; eof = 1; break; }
            if (!count) { eof = 1; break; }
            received += count;
            if (received == sizeof(packet)) {
                /* The peer is our forked worker, but still reject malformed
                 * framing instead of reading past a telemetry string. */
                packet.message[sizeof(packet.message) - 1] = 0;
                packet.detail[sizeof(packet.detail) - 1] = 0;
                progress = packet.progress > 100 ? 100 : packet.progress;
                snprintf(message, sizeof(message), "%s", packet.message);
                snprintf(detail, sizeof(detail), "%s", packet.detail);
                if (packet.complete == 1) { final = packet; complete = 1; }
                received = 0;
            }
        }
        if (!exited) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) exited = 1;
            else if (result < 0 && errno != EINTR) { exited = 1; read_error = errno; }
        }
        if (exited && eof) break;
        tui_begin(&view, T("Operation in progress"));
        char bar[96], fill[51];
        memset(fill, ' ', 50);
        memset(fill, '#', progress / 2);
        fill[50] = 0;
        snprintf(bar, sizeof(bar), "[%s] %3u%%", fill, progress);
        tui_text(&view, bar); tui_text(&view, message); tui_text(&view, detail);
        tui_text(&view, T("Disk operations are running. Please wait for completion."));
        tui_text(&view, log_path);
        tui_button(&view, T("Show / hide log (F4)"), TUI_LOG);
        char log[8192];
        if (show_log) { tui_read_log(log_path, log, sizeof(log)); tui_document(&view, log); }
        if (installer_tui_signal) snprintf(view.status, sizeof(view.status), "%s", T("The terminal requested exit. Waiting for the disk operation to finish."));
        if (tui_step(&view, NULL, 0, 0, 100) == TUI_LOG) show_log = !show_log;
    }
    close(descriptors[0]);
    if (read_error || received || !complete || !WIFEXITED(status) || WEXITSTATUS(status))
        return job_error(read_error ? -read_error : -EIO);
    *context->selected_partition = final.selected;
    *context->root_partition = final.root;
    *context->esp_partition = final.esp;
    *context->partition_auto = final.automatic;
    installer_partition_table_replaced = final.replaced;
    context->setup->format_esp = final.format_esp;
    if (job->kind == TUI_JOB_INSTALL) *context->install_success = final.success;
    job->info = final.info;
    copy_text(status_text, sizeof(status_text), final.message);
    copy_text(detail_text, sizeof(detail_text), final.detail);
    if (final.result) {
        snprintf(detail_text, sizeof(detail_text), "%s (%d)", strerror(-final.result), final.result);
        size_t position = strlen(detail_text);
        if (position + 1 < sizeof(detail_text)) {
            detail_text[position++] = '\n';
            copy_text(detail_text + position, sizeof(detail_text) - position, final.detail);
        }
    }
    return final.result;
}
