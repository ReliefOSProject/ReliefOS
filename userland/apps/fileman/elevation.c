/* Privileged edits require an external terminal; there is no GUI broker. */
#include "fileman.h"

char fileman_elevated_path[RELIEFOS_FS_PATH_LEN];

void fileman_forget_elevation(void) { fileman_elevated_path[0] = 0; }
int fileman_elevation_applies(const char *path)
{
    return path && path[0] && text_eq(path, fileman_elevated_path);
}
int fileman_prompt_elevation(const char *path)
{
    (void)path;
    set_status(T("Permission denied; administrator access requires a terminal in X11"));
    return -1;
}
int fileman_list_elevated(const char *path, struct reliefos_dir_entry *out,
                          uint32_t capacity, uint32_t *count)
{
    (void)path; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
}
int fileman_mkdir_elevated(const char *parent, const char *name,
                           struct reliefos_dir_entry *out, uint32_t capacity, uint32_t *count)
{
    (void)parent; (void)name; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
}
int fileman_rename_elevated(const char *from, const char *to,
                            struct reliefos_dir_entry *out, uint32_t capacity, uint32_t *count)
{
    (void)from; (void)to; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
}
int fileman_delete_elevated(const char *path, uint8_t is_dir,
                            struct reliefos_dir_entry *out, uint32_t capacity, uint32_t *count)
{
    (void)path; (void)is_dir; (void)out; (void)capacity; (void)count;
    return fileman_prompt_elevation(NULL);
}
