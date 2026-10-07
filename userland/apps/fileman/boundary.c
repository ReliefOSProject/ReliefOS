#include "boundary.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

int fileman_name_valid(const char *name)
{
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (const unsigned char *at = (const unsigned char *)name; *at; ++at)
        if (*at == '/' || *at < 32 || *at == 127) return 0;
    return 1;
}

int fileman_join_path(char *path, size_t capacity, const char *parent, const char *name)
{
    if (!path || !capacity) return -1;
    path[0] = 0;
    if (!parent || !*parent || !fileman_name_valid(name)) return -1;
    size_t length = strlen(parent);
    while (length > 1 && parent[length - 1] == '/') --length;
    if (length + strlen(name) + 2 > capacity) return -1;
    memcpy(path, parent, length);
    if (length != 1 || parent[0] != '/') path[length++] = '/';
    strcpy(path + length, name);
    return 0;
}

int fileman_path_contains(const char *parent, const char *path)
{
    if (!parent || !*parent || !path) return 0;
    size_t length = strlen(parent);
    if (!strcmp(parent, "/")) return path[0] == '/';
    return !strncmp(parent, path, length) && (!path[length] || path[length] == '/');
}

const char *fileman_x11_default_app(const char *path)
{
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    const char *extension = strrchr(base ? base + 1 : path, '.');
    if (extension && (!strcasecmp(extension, ".elf") || !strcasecmp(extension, ".lnk")))
        return NULL;
    if (extension && (!strcasecmp(extension, ".html") || !strcasecmp(extension, ".htm")))
        return "/usr/bin/dillo";
    return "/usr/bin/nedit";
}
