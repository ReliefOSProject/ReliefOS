/* Installer wizard model: toolkit-independent page flow and formatting. */
#ifndef INSTALLER_MODEL_H
#define INSTALLER_MODEL_H

#include <stdint.h>

enum installer_page {
    INSTALLER_PAGE_LANGUAGE = 0,
    INSTALLER_PAGE_THANKS,
    INSTALLER_PAGE_THEME,
    INSTALLER_PAGE_WELCOME,
    INSTALLER_PAGE_MODE,
    INSTALLER_PAGE_DISK,
    INSTALLER_PAGE_ACCOUNTS,
    INSTALLER_PAGE_CONFIRM,
    INSTALLER_PAGE_PROGRESS,
    INSTALLER_PAGE_FINISH,
    INSTALLER_PAGE_COUNT,
};

enum installer_mode {
    INSTALLER_MODE_FRESH = 0,
    INSTALLER_MODE_UPDATE = 1,
};

enum installer_action {
    INSTALLER_ACTION_NEXT = 0,
    INSTALLER_ACTION_INSTALL,
    INSTALLER_ACTION_UPDATE,
    INSTALLER_ACTION_RESTART,
    INSTALLER_ACTION_CLOSE,
};

/* Forward transition; the disk page branches by setup mode (update goes
 * straight to the confirm page, fresh installs collect accounts first). */
enum installer_page installer_model_next(enum installer_page page,
                                         enum installer_mode mode);

/* Backward transition; the confirm page returns to the page that opened it
 * (the disk page in update mode, accounts in fresh mode). */
enum installer_page installer_model_prev(enum installer_page page,
                                         enum installer_mode mode);

/* Back is disabled on the first page while installing and after success. */
int installer_model_can_go_back(enum installer_page page, int install_success);

/* Cancel is disabled while installing and after a successful finish. */
int installer_model_can_cancel(enum installer_page page, int install_success);

/* Primary footer action for the page. */
enum installer_action installer_model_action(enum installer_page page,
                                             enum installer_mode mode,
                                             int install_success);

/* The exact word the confirm page requires before destructive writes. */
const char *installer_model_confirm_word(enum installer_mode mode);

/* Fill `out` with the sidebar step pages for the mode in display order;
 * returns the step count. At most INSTALLER_PAGE_COUNT entries are written. */
int installer_model_steps(enum installer_mode mode,
                          enum installer_page *out, int cap);

/* "Disk <id>  <name>  <size> GiB|MiB" for a disk list row. */
void installer_model_format_disk_line(char *buf, uint32_t cap, uint32_t id,
                                      const char *name, uint64_t sector_count,
                                      uint32_t sector_size);

int installer_model_secret_characters(const char *text, uint32_t length);
int installer_model_edit_secret(char *secret, uint32_t cap, uint32_t start,
                                uint32_t end, const char *text, uint32_t length);

#endif
