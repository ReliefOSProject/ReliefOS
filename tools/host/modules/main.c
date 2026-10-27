/* SPDX-License-Identifier: Apache-2.0 */
#include "tools/host/modules/bundle.h"
#include "tools/host/modules/stage.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "modules-stage")) return kernel_modules_stage(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "initramfs-stage")) return kernel_initramfs_stage(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "kernel-payload")) return kernel_payload_stage(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "boot-payload")) return kernel_boot_stage(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "check-stage")) return kernel_stage_check(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "slot-encode")) return kernel_slot_encode_command(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "slot-select")) return kernel_slot_select_command(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "release-info")) return kernel_release_info(argc - 2, argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "check-install")) {
        struct kernel_bundle bundle = {.root_fd = -1};
        int result = kernel_bundle_open(&bundle, argv[2], (size_t)(argc - 3),
                                         (const char *const *)(argv + 3));
        if (result)
            fprintf(stderr, "kernel-tools: %s\n", bundle.error);
        else
            printf("PASS installed kernel bundle %s files=%zu modules=%zu\n",
                   bundle.release, bundle.file_count, bundle.module_count);
        kernel_bundle_close(&bundle);
        return result ? 1 : 0;
    }
    fputs("usage: reliefos-kernel-tools COMMAND ARGS...\n"
          "commands: check-install, modules-stage, initramfs-stage, kernel-payload, boot-payload, check-stage, slot-encode, slot-select, release-info\n", stderr);
    return 2;
}
