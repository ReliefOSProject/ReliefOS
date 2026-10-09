/* Installer install/update operations shared by the Motif and TTY frontends. */
#ifndef RELIEFOS_INSTALLER_OPS_H
#define RELIEFOS_INSTALLER_OPS_H

#include <reliefos/blockdev.h>
#include <reliefos/layout.h>
#include <stdint.h>

#include "installer_setup.h"
#include "model.h"

/* The operations code drives the wizard state under the historical page and
 * mode names; the frontends use the model enums directly. */
#define PAGE_CONFIRM INSTALLER_PAGE_CONFIRM
#define PAGE_PROGRESS INSTALLER_PAGE_PROGRESS
#define PAGE_FINISH INSTALLER_PAGE_FINISH
#define INSTALL_MODE_FRESH INSTALLER_MODE_FRESH
#define INSTALL_MODE_UPDATE INSTALLER_MODE_UPDATE

/* Values written to the installed system's display.conf theme= line. */
#define INSTALLER_THEME_WIN95 0u
#define INSTALLER_THEME_METRO 1u

#define COPY_BUF_SIZE (32U * 1024U)
#define COPY_ESP_WRITE_SLICE 4096U
#define COPY_PRESENT_INTERVAL_MS 50U
#define INSTALL_ROOT_PAYLOAD "/install/root"
#define INSTALL_ESP_PAYLOAD "/install/esp"
#define INSTALL_ROOT_MOUNT "/target"
#define INSTALL_ESP_MOUNT "/target/boot"

/* Target-root path contract.  The installer payload uses the same relative
 * paths as the guest root, so each payload path is INSTALL_ROOT_PAYLOAD plus
 * the matching guest-relative name. */
#define TARGET_RELIEFOS_APPS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_APPS
#define TARGET_RELIEFOS_LIB INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_LIB
#define TARGET_RELIEFOS_DRIVERS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_DRIVERS
#define TARGET_RELIEFOS_DOC INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_DOC
#define TARGET_ETC_RELIEFOS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_ETC_RELIEFOS
#define TARGET_VAR_LIB_RELIEFOS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS
#define TARGET_VAR_LOG INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_VAR_LOG
#define TARGET_VAR_TMP INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_VAR_TMP
#define TARGET_FONTS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_FONTS
#define TARGET_RESOURCES INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RELIEFOS_RESOURCES
#define TARGET_CERTS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_ETC_SSL_CERTS
#define TARGET_USR_BIN INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_BIN
#define TARGET_USR_SBIN INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_SBIN
#define TARGET_USR_LIB INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_LIB
#define TARGET_USR_SHARE INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_USR_SHARE
#define TARGET_HOME INSTALL_ROOT_MOUNT "/home"
#define TARGET_OPT INSTALL_ROOT_MOUNT "/opt"
#define TARGET_RUN_RELIEFOS INSTALL_ROOT_MOUNT RELIEFOS_LAYOUT_RUN_RELIEFOS
#define TARGET_BOOT INSTALL_ROOT_MOUNT "/boot"
#define TARGET_ESP_KERNEL TARGET_BOOT "/reliefos/kernel.sys"
#define TARGET_ESP_KERNEL_LEGACY TARGET_BOOT "/leonos/kernel.sys"

/* Wizard state shared with the frontends. */
extern uint8_t page;
extern uint8_t install_mode;
extern uint8_t installer_theme;
extern uint8_t installer_theme_explicit;
extern struct reliefos_block_disk_info disks[RELIEFOS_BLOCK_MAX_DISKS];
extern uint32_t disk_count;
extern int32_t selected_disk;
extern char confirm_text[16];
extern struct installer_setup setup;
extern char status_text[128];
extern char detail_text[128];
extern char progress_text[256];
extern uint32_t progress_value;
extern uint8_t install_success;
extern uint8_t install_running;
extern int reboot_error;
extern uint8_t installer_tty_mode;

/* Implemented by the frontend: repaint from the shared state.  Long-running
 * install operations call this between stages; the TTY frontend no-ops it. */
void refresh_ui(void);

int text_eq(const char *a, const char *b);
void copy_text(char *dst, uint32_t cap, const char *src);
void set_status(const char *status, const char *detail);
void set_error_status(const char *prefix, int ret);
const char *mode_action_text(void);
const char *mode_progress_title(void);
void set_disk_select_status(void);
void format_disk_line(char *buf, uint32_t cap,
                      const struct reliefos_block_disk_info *disk);
void reset_confirm(void);
int confirmation_ok(void);
void refresh_disks(void);

/* Long-running operations; they drive `page` to PROGRESS then FINISH. */
void prepare_update_target(void);
void perform_install(void);
void perform_update(void);

#endif
