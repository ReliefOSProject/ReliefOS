#include <stdio.h>
#include <string.h>

extern int installer_swap_storage_supported_at(const char *,unsigned,unsigned) __attribute__((weak));

int main(int argc,char **argv)
{
    if (argc!=4) return 2;
    if (!installer_swap_storage_supported_at) {
        fputs("FAIL: selected-disk sysfs preflight is absent\n",stderr);return 1;
    }
    int rc=installer_swap_storage_supported_at(argv[1],8,0);
    if ((!strcmp(argv[2],"yes") && !rc) || (!strcmp(argv[2],"no") && rc<0)) return 0;
    fprintf(stderr,"FAIL: %s expected %s result=%d\n",argv[3],argv[2],rc);
    return 1;
}
