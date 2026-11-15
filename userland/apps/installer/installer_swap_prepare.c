#include "installer_swap.h"
#include "installer_capacity.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/** @brief Prepare optional swap and publish fresh target configuration.
 * @param root Absolute owned target. @param root_uuid Root GPT UUID. @param esp_uuid
 * Boot GPT UUID. @param filesystem Selected ext3 or ext4 root. @param mib Selected size or zero to disable. @param progress
 * Optional synchronous callback. @param context Borrowed callback context.
 * @return Zero or negative errno, with cleanup errors reported separately.
 */
int installer_swap_prepare_fs(const char *root,const char *root_uuid,const char *esp_uuid,
                              const char *filesystem,uint64_t mib,
                              installer_swap_progress_fn progress,void *context)
{
    struct installer_swap_identity identity;
    if (!filesystem || (strcmp(filesystem,"ext3") && strcmp(filesystem,"ext4"))) return -EINVAL;
    if (mib && !installer_swap_size_valid(mib)) return -EINVAL;
    int rc=0;
    if (mib) rc=installer_swap_create_owned(root,mib*1024u*1024u,progress,context,&identity);
    if (rc) return rc;
    bool retain_file=false;
    rc=installer_swap_configure_fs(root,root_uuid,esp_uuid,filesystem,mib!=0,&retain_file);
    if (rc && mib && !retain_file) {
        int cleanup=installer_swap_remove_owned(root,&identity);
        if (cleanup) fprintf(stderr,"[installer.elf] swapfile rollback failed: %s\n",strerror(-cleanup));
    } else if (rc && mib && retain_file)
        fprintf(stderr,"[installer.elf] retained swapfile: fstab reference or rollback durability is uncertain\n");
    return rc;
}

/** @brief Keep the default ext4 preparation entry point for existing callers.
 * @param root Fresh target. @param root_uuid Root UUID. @param esp_uuid Boot UUID.
 * @param mib Swap MiB or zero. @param progress Optional callback. @param context Callback data.
 * @return Zero or negative errno.
 */
int installer_swap_prepare(const char *root,const char *root_uuid,const char *esp_uuid,
                           uint64_t mib,installer_swap_progress_fn progress,void *context)
{ return installer_swap_prepare_fs(root,root_uuid,esp_uuid,"ext4",mib,progress,context); }
