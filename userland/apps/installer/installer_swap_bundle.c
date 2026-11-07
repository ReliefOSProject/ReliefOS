#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "installer_swap.h"
#include <reliefos/kernel_bundle.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Validate the selected payload's exact release and swap configuration.
 * @param payload Absolute target payload root. @return Zero when its complete
 * validated bundle enables swap, or negative lookup, validation or I/O errno.
 */
int installer_swap_bundle_supported(const char *payload)
{
    if (!payload || payload[0]!='/') return -EINVAL;
    int directory=open(payload,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (directory<0) return -errno;
    const char *parts[]={"usr","lib","reliefos","kernel"};
    int result=0;
    for (unsigned i=0;i<sizeof(parts)/sizeof(parts[0]);++i) {
        int next=openat(directory,parts[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (next<0) {result=-errno;goto out;}
        close(directory);directory=next;
    }
    int fd=openat(directory,"current-release",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) {result=-errno;goto out;}
    struct stat st;
    if (fstat(fd,&st)<0) {result=-errno;close(fd);goto out;}
    if (!S_ISREG(st.st_mode) || st.st_size<2 || st.st_size>32) {
        result=-EINVAL;close(fd);goto out;
    }
    char release[33];size_t used=0,length=(size_t)st.st_size;
    while (used<length) {
        ssize_t count=read(fd,release+used,length-used);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) {result=count<0 ? -errno : -EIO;break;}
        used+=(size_t)count;
    }
    if (close(fd)<0 && !result) result=-errno;
    if (result) goto out;
    if (release[length-1]!='\n' || memchr(release,0,length) || memchr(release,'\n',length-1)) {
        result=-EINVAL;goto out;
    }
    release[length-1]=0;
    int enabled=0;
    result=reliefos_kernel_bundle_swap_supported(payload,release,&enabled);
    if (!result && !enabled) result=-EOPNOTSUPP;
out:
    if (close(directory)<0 && !result) result=-errno;
    return result;
}
