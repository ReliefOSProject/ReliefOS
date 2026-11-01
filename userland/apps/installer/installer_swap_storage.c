/* Resolve standard sysfs hierarchy from the selected disk's real dev_t. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "installer_swap.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/** @brief Append one bounded path component.
 * @param out Writable destination. @param size Destination capacity in bytes.
 * @param base Existing path. @param suffix Component suffix.
 * @return Zero or negative ENAMETOOLONG.
 */
static int path_join(char *out,size_t size,const char *base,const char *suffix)
{
    int n=snprintf(out,size,"%s/%s",base,suffix);
    return n<0 || (size_t)n>=size ? -ENAMETOOLONG : 0;
}

/** @brief Resolve a dev_t to a verified PCI AHCI ancestor under one sysfs root.
 * @param sysroot Absolute sysfs root. @param major_number Block major.
 * @param minor_number Block minor. @return Zero for AHCI or negative errno.
 */
int installer_swap_storage_supported_at(const char *sysroot,unsigned major_number,unsigned minor_number)
{
    if (!sysroot) return -EINVAL;
    char root[PATH_MAX],devices[PATH_MAX],pci[PATH_MAX],link[PATH_MAX],current[PATH_MAX];
    if (!realpath(sysroot,root)) return -errno;
    int rc=path_join(devices,sizeof(devices),root,"devices");
    if (!rc) rc=path_join(pci,sizeof(pci),root,"bus/pci");
    char devname[64];
    int n=snprintf(devname,sizeof(devname),"dev/block/%u:%u",major_number,minor_number);
    if (n<0 || (size_t)n>=sizeof(devname)) return -EOVERFLOW;
    if (!rc) rc=path_join(link,sizeof(link),root,devname);
    if (rc) return rc;
    if (!realpath(link,current)) return -errno;
    size_t prefix=strlen(devices);
    if (strncmp(current,devices,prefix) || current[prefix]!='/') return -EOPNOTSUPP;
    for (;;) {
        char subsystem[PATH_MAX],resolved[PATH_MAX];
        rc=path_join(subsystem,sizeof(subsystem),current,"subsystem");
        if (rc) return rc;
        if (realpath(subsystem,resolved) && !strcmp(resolved,pci)) {
            char class_path[PATH_MAX],line[64];
            rc=path_join(class_path,sizeof(class_path),current,"class");
            if (rc) return rc;
            FILE *file=fopen(class_path,"r");
            if (!file) return -errno;
            int good=fgets(line,sizeof(line),file)!=NULL && !strcmp(line,"0x010601\n");
            if (fclose(file)) return -errno;
            return good ? 0 : -EOPNOTSUPP;
        }
        char *slash=strrchr(current,'/');
        if (!slash || (size_t)(slash-current)<prefix) return -EOPNOTSUPP;
        *slash=0;
        if (!strcmp(current,devices)) return -EOPNOTSUPP;
    }
}

/** @brief Check the selected block device against the running sysfs topology.
 * @param disk_path Selected device path. @return Zero for AHCI or negative errno.
 */
int installer_swap_storage_supported(const char *disk_path)
{
    struct stat st;
    if (!disk_path) return -EINVAL;
    if (stat(disk_path,&st)<0) return -errno;
    if (!S_ISBLK(st.st_mode)) return -ENOTBLK;
    return installer_swap_storage_supported_at("/sys",major(st.st_rdev),minor(st.st_rdev));
}
