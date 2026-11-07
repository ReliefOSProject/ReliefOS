/* DOOM launcher model: toolkit-independent argument assembly. */
#ifndef DOOMLAUNCHER_MODEL_H
#define DOOMLAUNCHER_MODEL_H

#include <stdint.h>

#define DOOMLAUNCHER_MAX_ARGS 16U
#define DOOMLAUNCHER_IWAD_CAP 256U
#define DOOMLAUNCHER_EXTRA_CAP 128U

enum doomlauncher_error {
    DOOMLAUNCHER_ERR_NO_IWAD = -1,
    DOOMLAUNCHER_ERR_TOO_MANY_ARGS = -2,
};

struct doomlauncher_options {
    char iwad[DOOMLAUNCHER_IWAD_CAP];
    char extra[DOOMLAUNCHER_EXTRA_CAP];
    uint8_t disable_sound;
    uint8_t fullscreen;
};

/* Reset to launcher defaults: default IWAD, no extras, sound off, windowed. */
void doomlauncher_defaults(struct doomlauncher_options *options,
                           const char *default_iwad);

/*
 * Assemble "doom.elf -iwad <path> [-nosound] [-windowed] <extras...>".
 * extra_argv holds the already-split extra arguments. Returns the argument
 * count (argv is NULL-terminated) or a negative doomlauncher_error value.
 */
int doomlauncher_build_argv(const char *doom_path,
                            const struct doomlauncher_options *options,
                            char *const extra_argv[], int extra_count,
                            char *argv[], uint32_t max_args);

/* Map a wait4() result to the child exit code, or -1 when it was not reaped. */
int doomlauncher_exit_code(int reaped, int exit_status);

#endif
