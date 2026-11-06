/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_XDG_H
#define RELIEFOS_XDG_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a standard user directory (DESKTOP, DOWNLOAD, DOCUMENTS, MUSIC,
 * PICTURES, VIDEOS, TEMPLATES or PUBLICSHARE). Read user-dirs.dirs as data;
 * expand only a leading $HOME and never evaluate shell commands. An unset or
 * disabled directory resolves to HOME, as xdg-user-dir does. Return 0 or
 * negative errno; the output is empty on error. No directory is created. */
int reliefos_xdg_user_dir(const char *kind, char *path, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
