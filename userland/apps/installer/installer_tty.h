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
    struct reliefos_block_partition *partitions;
    uint32_t *partition_count;
    int32_t *selected_partition;
    int32_t *root_partition;
    int32_t *esp_partition;
    uint8_t *partition_auto;
    int32_t *selected_disk;
    uint8_t *install_mode;
    uint8_t *install_success;
    uint8_t *page;
    void (*refresh_disks)(void);
    void (*format_disk_line)(char *buf, uint32_t cap,
                             const struct reliefos_block_disk_info *disk);
    void (*refresh_partitions)(void);
    void (*format_partition_line)(char *buf, uint32_t cap,
                                  const struct reliefos_block_partition *partition);
    int (*partition_plan_valid)(void);
    int (*partition_auto_layout)(void);
    int (*partition_initialize)(void);
    int (*partition_create)(uint32_t filesystem, uint32_t size_mib, const char *name);
    int (*partition_resize)(uint32_t size_mib);
    int (*partition_rename)(const char *name);
    int (*partition_set_type)(uint32_t type);
    int (*partition_delete)(void);
    int (*partition_format)(uint32_t filesystem, const char *label);
    void (*prepare_update)(void);
    void (*perform_install)(void);
    void (*perform_update)(void);
};

int installer_tty_main(const struct installer_tty_context *context);

#endif
