#include "../../userland/apps/installer/installer_swap.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static int create_calls,configure_calls,remove_calls,mode;
int installer_swap_create_owned(const char *root,uint64_t bytes,installer_swap_progress_fn cb,
                                void *context,struct installer_swap_identity *id)
{
    assert(!strcmp(root,"/target") && bytes==512ULL*1024*1024 && !cb && !context);
    ++create_calls;id->file_ino=42;return mode==1 ? -ENOSPC : 0;
}
int installer_swap_configure(const char *root,const char *ru,const char *eu,bool enabled,bool *retain_file)
{
    assert(!strcmp(root,"/target") && !strcmp(ru,"root") && !strcmp(eu,"esp"));
    ++configure_calls;assert(enabled==(mode!=4));*retain_file=mode==3;
    return mode==2 || mode==3 ? -EIO : 0;
}
int installer_swap_remove_owned(const char *root,const struct installer_swap_identity *id)
{assert(!strcmp(root,"/target") && id->file_ino==42);++remove_calls;return 0;}
int main(void)
{
    for (mode=0;mode<=4;++mode) {
        create_calls=configure_calls=remove_calls=0;
        int rc=installer_swap_prepare("/target","root","esp",mode==4 ? 0 : 512,NULL,NULL);
        assert(rc==(mode==1 ? -ENOSPC : mode==2 || mode==3 ? -EIO : 0));
        assert(create_calls==(mode!=4));assert(configure_calls==(mode!=1));
        assert(remove_calls==(mode==2));
    }
    puts("PASS: swap preparation orders creation, configuration and identity-checked rollback");return 0;
}
