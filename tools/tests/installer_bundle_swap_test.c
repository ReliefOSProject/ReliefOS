#include <errno.h>
#include <stdio.h>
#include <string.h>

extern int reliefos_kernel_bundle_swap_supported(const char *,const char *,int *) __attribute__((weak));
extern int installer_swap_bundle_supported(const char *) __attribute__((weak));

int main(int argc,char **argv)
{
    if (argc!=4) return 2;
    if (!reliefos_kernel_bundle_swap_supported) {
        fputs("FAIL: verified bundle swap capability is absent\n",stderr);return 1;
    }
    if (!installer_swap_bundle_supported) {
        fputs("FAIL: selected payload preflight is absent\n",stderr);return 1;
    }
    int enabled=-1;
    int rc=reliefos_kernel_bundle_swap_supported(argv[1],"5.1.2-ko1-aaaaaaaaaaaa",&enabled);
    int selected=installer_swap_bundle_supported(argv[1]);
    if (!strcmp(argv[2],"yes") && !rc && enabled==1 && !selected) return 0;
    if (!strcmp(argv[2],"no") && !rc && enabled==0 && selected<0) return 0;
    if (!strcmp(argv[2],"error") && rc<0 && enabled==-1 && selected<0) return 0;
    fprintf(stderr,"FAIL: %s expected %s rc=%d enabled=%d\n",argv[3],argv[2],rc,enabled);
    return 1;
}
