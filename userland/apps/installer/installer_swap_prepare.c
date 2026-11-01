#include "installer_swap.h"
#include "installer_capacity.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/** @brief Prepare optional swap and publish fresh target configuration.
 * @param root Absolute owned target. @param root_uuid Root GPT UUID. @param esp_uuid
 * Boot GPT UUID. @param mib Selected size or zero to disable. @param progress
 * Optional synchronous callback. @param context Borrowed callback context.
 * @return Zero or negative errno, with cleanup errors reported separately.
 */
int installer_swap_prepare(const char *root,const char *root_uuid,const char *esp_uuid,
                           uint64_t mib,installer_swap_progress_fn progress,void *context)
{
    struct installer_swap_identity identity;
    if (mib && !installer_swap_size_valid(mib)) return -EINVAL;
    int rc=0;
    if (mib) rc=installer_swap_create_owned(root,mib*1024u*1024u,progress,context,&identity);
    if (rc) return rc;
    bool retain_file=false;
    rc=installer_swap_configure(root,root_uuid,esp_uuid,mib!=0,&retain_file);
    if (rc && mib && !retain_file) {
        int cleanup=installer_swap_remove_owned(root,&identity);
        if (cleanup) fprintf(stderr,"[installer.elf] swapfile rollback failed: %s\n",strerror(-cleanup));
    } else if (rc && mib && retain_file)
        fprintf(stderr,"[installer.elf] retained swapfile: fstab reference or rollback durability is uncertain\n");
    return rc;
}
