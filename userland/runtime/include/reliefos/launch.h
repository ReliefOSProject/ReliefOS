#ifndef RELIEFOS_LAUNCH_H
#define RELIEFOS_LAUNCH_H

#include <stdint.h>

void reliefos_launch_use_session(int enabled);

#define RELIEFOS_LAUNCH_MAX_ARGS 8U

#define RELIEFOS_LAUNCH_ERR_EMPTY -1001
#define RELIEFOS_LAUNCH_ERR_TOO_MANY_ARGS -1002
#define RELIEFOS_LAUNCH_ERR_UNCLOSED_QUOTE -1003
#define RELIEFOS_LAUNCH_ERR_NOT_FOUND -1004
#define RELIEFOS_LAUNCH_ERR_NO_ASSOCIATION -1005
#define RELIEFOS_LAUNCH_ERR_INVALID_SHORTCUT -1006
#define RELIEFOS_LAUNCH_ERR_SHORTCUT_LOOP -1007
#define RELIEFOS_LAUNCH_ERR_EXISTS -1008
#define RELIEFOS_LAUNCH_ERR_ALREADY_RUNNING -1009
#define RELIEFOS_LAUNCH_ASSOC_COUNT 7U

struct reliefos_launch_assoc_app {
    const char *name;
    const char *detail;
    const char *program_path;
    uint8_t mode;
};

#define RELIEFOS_LAUNCH_ASSOC_MODE_EXEC 1U
#define RELIEFOS_LAUNCH_ASSOC_MODE_OPEN_TEXT 2U
#define RELIEFOS_LAUNCH_ASSOC_MODE_TERMINAL_CAT 3U

int reliefos_cmdline_split(char *line, char *argv[], uint32_t max_args);
const char *reliefos_launch_builtin_path(const char *name_or_path);
int reliefos_launch_file_with_app(const char *target_path, const char *program_path);
const char *reliefos_launch_resolve_default_app_for_path(const char *path);
const char *reliefos_launch_get_extension_for_path(const char *path, char *buffer,
                                                 uint32_t capacity);
const struct reliefos_launch_assoc_app *reliefos_launch_assoc_apps(uint32_t *count);
int reliefos_launch_set_extension_association(const char *extension, const char *program_path);
int reliefos_launch_get_extension_association(const char *extension, char *program_path,
                                            uint32_t capacity);
void reliefos_launch_default_shortcut_name(const char *target_path, char *buffer,
                                         uint32_t capacity);
/* Application shortcuts are standard .desktop files; other paths use POSIX
 * symbolic links. Never overwrite an existing destination. */
int reliefos_launch_create_shortcut(const char *shortcut_path, const char *target_path);
int reliefos_launch_create_shortcut_in_dir(const char *dir_path, const char *target_path,
                                         char *out_path, uint32_t out_capacity);
/* Launch a Desktop Entry through the packaged GLib gio implementation. Field
 * codes, quoting, localization, Path and Terminal are handled by upstream. */
int reliefos_launch_desktop_file(char *const arguments[]);
/* Starts an executable in a child process. execve() intentionally replaces
 * the current process image, so graphical launchers must use this helper. */
int reliefos_spawn_argv(const char *path, char *const argv[]);
int reliefos_launch_argv(char *argv[]);
int reliefos_launch_command_line(char *line, char *argv[], uint32_t max_args);
const char *reliefos_launch_error_text(int code);

#endif
