#ifndef RELIEFOS_INSTALLER_TUI_H
#define RELIEFOS_INSTALLER_TUI_H

#include <stddef.h>
#include <signal.h>
#include "installer_tty.h"
#include "installer_update.h"
#include "installer_tui_partitions.h"

#ifdef __cplusplus
extern "C" {
#endif

enum tui_action { TUI_NONE = -1, TUI_BACK = 1000, TUI_NEXT, TUI_CANCEL, TUI_LOG };
enum tui_kind { TUI_TEXT, TUI_BUTTON, TUI_RADIO, TUI_CHECK, TUI_INPUT, TUI_PARTITIONS };
struct tui_item {
    enum tui_kind kind;
    char label[768];
    const char *document;
    char *value;
    size_t capacity;
    int *selection;
    struct tui_partition_table *table;
    int option, secret, action, row, height, compact, column, width;
};
struct tui_view {
    char title[128], status[768];
    struct tui_item items[96];
    int count, focus, scroll, cursor, previous_focus;
};

extern volatile sig_atomic_t installer_tui_signal;
int tui_open(void);
void tui_close(void);
void tui_begin(struct tui_view *view, const char *title);
void tui_text(struct tui_view *view, const char *text);
void tui_document(struct tui_view *view, const char *text);
void tui_button(struct tui_view *view, const char *label, int action);
void tui_radio(struct tui_view *view, const char *label, int *selection, int option);
void tui_check(struct tui_view *view, const char *label, int *selection);
void tui_input(struct tui_view *view, const char *label, char *value, size_t capacity, int secret);
/** @brief Add a keyboard-focusable partition table using a borrowed snapshot.
 * @param view Caller-owned page being assembled.
 * @param table Snapshot and selection state, live until the next tui_step returns.
 */
void tui_partitions(struct tui_view *view, struct tui_partition_table *table);
/** @brief Add a button to a wrapping horizontal action row.
 * @param view Caller-owned page. @param label Borrowed UTF-8 label, copied internally.
 * @param action Value returned when the user activates this button.
 */
void tui_compact_button(struct tui_view *view, const char *label, int action);
/** @brief Add a radio option to a wrapping horizontal control row.
 * @param view Caller-owned page. @param label Borrowed UTF-8 label, copied internally.
 * @param selection Borrowed mutable selection. @param option Value selected by activation.
 */
void tui_compact_radio(struct tui_view *view, const char *label, int *selection, int option);
int tui_step(struct tui_view *view, const char *primary, int back, int cancel, int timeout_ms);
int tui_confirm(const char *title, const char *message, const char *word);
int tui_edit(const char *title, const char *label, char *text, size_t capacity);
int tui_pick(const char *title, const char *const *options, int count, int initial);
void tui_message(const char *title, const char *message);
void tui_read_log(const char *path, char *text, size_t capacity);
int tui_edit_value(char *value, size_t capacity, int *cursor, unsigned key, int special, int secret);

enum tui_job_kind {
    TUI_JOB_INSTALL, TUI_JOB_CHECK, TUI_JOB_UPDATE, TUI_JOB_AUTO, TUI_JOB_INIT,
    TUI_JOB_CREATE, TUI_JOB_RESIZE, TUI_JOB_RENAME, TUI_JOB_FORMAT, TUI_JOB_DELETE,
    TUI_JOB_ROLE
};
struct tui_job {
    enum tui_job_kind kind;
    unsigned filesystem, size, role;
    char name[128], root_uuid[37], esp_uuid[37];
    struct installer_update_info info;
};
int tui_run_job(const struct installer_tty_context *context, struct tui_job *job);
void installer_tui_publish_progress(void);

#ifdef __cplusplus
}
#endif

#endif
