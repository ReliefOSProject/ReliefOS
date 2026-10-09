#ifndef RELIEFOS_INSTALLER_TTY_H
#define RELIEFOS_INSTALLER_TTY_H

#include <reliefos/blockdev.h>
#include <stdint.h>
#include "installer_setup.h"

enum installer_tty_install_mode {
    INSTALLER_TTY_MODE_INSTALL = 0,
    INSTALLER_TTY_MODE_UPDATE = 1,
};

struct installer_tty_context {
    struct installer_setup *setup;
    struct reliefos_block_disk_info *disks;
    uint32_t *disk_count;
    int32_t *selected_disk;
    uint8_t *install_mode;
    uint8_t *install_success;
    uint8_t *page;
    void (*refresh_disks)(void);
    void (*format_disk_line)(char *buf, uint32_t cap,
                             const struct reliefos_block_disk_info *disk);
    void (*prepare_update)(void);
    void (*perform_install)(void);
    void (*perform_update)(void);
};

int installer_tty_main(const struct installer_tty_context *context);

#endif
