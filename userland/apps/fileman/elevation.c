/* Each operation invokes upstream sudo and re-evaluates its command policy. */
#include "fileman.h"
#include <reliefos/sudo.h>

char fileman_elevated_path[RELIEFOS_FS_PATH_LEN];

void fileman_forget_elevation(void) { fileman_elevated_path[0] = 0; }
int fileman_elevation_applies(const char *path)
{
    return path && path[0] && text_eq(path, fileman_elevated_path);
}
int fileman_prompt_elevation(const char *path)
{
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    (void)path;
    set_status(T("Permission denied; administrator access requires a terminal in X11"));
    return -1;
#else
    uint32_t count;
    if (reliefos_fileop(RELIEFOS_FILEOP_LIST, path, NULL, NULL, NULL, entries,
                       FILEMAN_MAX_ENTRIES, &count) < 0) {
        set_status(T("Operation denied or canceled"));
        return -1;
    }
    copy_text(fileman_elevated_path, sizeof(fileman_elevated_path), path);
    return 0;
#endif
}
int fileman_list_elevated(const char *path, struct reliefos_dir_entry *out,
                          uint32_t capacity, uint32_t *count)
{
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    (void)path; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
#else
    return reliefos_fileop(RELIEFOS_FILEOP_LIST, path, NULL, NULL, NULL, out, capacity, count);
#endif
}
int fileman_mkdir_elevated(const char *parent, const char *name,
                           struct reliefos_dir_entry *out, uint32_t capacity, uint32_t *count)
{
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    (void)parent; (void)name; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
#else
    return reliefos_fileop(RELIEFOS_FILEOP_MKDIR, parent, name, NULL, NULL, out, capacity, count);
#endif
}
int fileman_rename_elevated(const char *from, const char *to,
                            struct reliefos_dir_entry *out, uint32_t capacity, uint32_t *count)
{
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    (void)from; (void)to; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
#else
    return reliefos_fileop(RELIEFOS_FILEOP_RENAME, from, to, NULL, NULL, out, capacity, count);
#endif
}
int fileman_delete_elevated(const char *path, uint8_t is_dir,
                            struct reliefos_dir_entry *out, uint32_t capacity, uint32_t *count)
{
#ifdef CONFIG_DESKTOP_BACKEND_XORG
    (void)path; (void)is_dir; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
#else
    return reliefos_fileop(RELIEFOS_FILEOP_UNLINK, path, is_dir ? RELIEFOS_FILEOP_CONFIRM : NULL,
                         NULL, NULL, out, capacity, count);
#endif
}
