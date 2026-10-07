#ifndef RELIEFOS_FILEMAN_BOUNDARY_H
#define RELIEFOS_FILEMAN_BOUNDARY_H
#include <stddef.h>
int fileman_name_valid(const char *name);
int fileman_join_path(char *path, size_t capacity, const char *parent, const char *name);
int fileman_path_contains(const char *parent, const char *path);
const char *fileman_x11_default_app(const char *path);
#endif
