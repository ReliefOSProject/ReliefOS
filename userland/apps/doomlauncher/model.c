#include "model.h"
#include <string.h>

static void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || !cap) return;
    while (src && src[i] && i + 1U < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

void doomlauncher_defaults(struct doomlauncher_options *options,
                           const char *default_iwad)
{
    if (!options) return;
    memset(options, 0, sizeof(*options));
    copy_text(options->iwad, sizeof(options->iwad), default_iwad);
    options->disable_sound = 1;
    options->fullscreen = 0;
}

int doomlauncher_build_argv(const char *doom_path,
                            const struct doomlauncher_options *options,
                            char *const extra_argv[], int extra_count,
                            char *argv[], uint32_t max_args)
{
    uint32_t argc = 0;
    uint32_t slots;
    if (!doom_path || !options || !argv || max_args < 4U) {
        return DOOMLAUNCHER_ERR_TOO_MANY_ARGS;
    }
    if (!options->iwad[0]) {
        return DOOMLAUNCHER_ERR_NO_IWAD;
    }
    if (extra_count < 0) extra_count = 0;
    /* doom.elf -iwad <path> plus optional flags, extras and the NULL
     * terminator must all fit into the caller's argv. */
    slots = 3U + (options->disable_sound ? 1U : 0U) +
            (options->fullscreen ? 0U : 1U) + (uint32_t)extra_count + 1U;
    if (slots > max_args) {
        return DOOMLAUNCHER_ERR_TOO_MANY_ARGS;
    }
    argv[argc++] = (char *)doom_path;
    argv[argc++] = "-iwad";
    argv[argc++] = (char *)options->iwad;
    if (options->disable_sound) argv[argc++] = "-nosound";
    if (!options->fullscreen) argv[argc++] = "-windowed";
    for (int i = 0; i < extra_count; ++i) {
        argv[argc++] = extra_argv[i];
    }
    argv[argc] = 0;
    return (int)argc;
}

int doomlauncher_exit_code(int reaped, int exit_status)
{
    return reaped > 0 ? ((exit_status >> 8) & 0xff) : -1;
}
