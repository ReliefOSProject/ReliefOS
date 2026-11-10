/* Exercise fresh-target fstab and OpenRC publication with real filesystem I/O. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int installer_swap_configure(const char *,const char *,const char *,bool,bool *) __attribute__((weak));
int __real_renameat(int,const char *,int,const char *);
int __real_fsync(int);
int __real_symlinkat(const char *,int,const char *);
int __real_close(int);
int __real_openat(int,const char *,int,...);
int __real_unlinkat(int,const char *,int);
static const char *which;
static bool faulted;
static bool fstab_renamed;
static bool sync_fault,close_fault;
static int etc_fd=-1;
int __wrap_fchown(int fd,uid_t uid,gid_t gid)
{ (void)fd;assert(!uid && !gid);return 0; }
int __wrap_openat(int dir,const char *name,int flags,...)
{
    mode_t mode=0;
    if (flags&O_CREAT) {va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
    int fd=__real_openat(dir,name,flags,mode);
    if (fd>=0 && !strcmp(name,"etc")) etc_fd=fd;
    return fd;
}
int __wrap_unlinkat(int fd,const char *name,int flags)
{
    if (!faulted && !strcmp(which,"backup-unlink") && !strcmp(name,".reliefos-fstab.backup")) {
        faulted=true;errno=EIO;return -1;
    }
    return __real_unlinkat(fd,name,flags);
}

int __wrap_renameat(int a,const char *x,int b,const char *y)
{
    if (!faulted && !strcmp(which,"fstab-rename") && !strcmp(y,"fstab")) {
        faulted=true;errno=EIO;return -1;
    }
    int result=__real_renameat(a,x,b,y);
    if (!result && !strcmp(y,"fstab")) fstab_renamed=true;
    return result;
}
int __wrap_symlinkat(const char *target,int fd,const char *name)
{
    if (!faulted && !strcmp(which,"link-fail") && !strcmp(name,"reliefos-swap-check")) {
        faulted=true;errno=EIO;return -1;
    }
    return __real_symlinkat(target,fd,name);
}
int __wrap_fsync(int fd)
{
    struct stat st;assert(!fstat(fd,&st));
    if (!sync_fault && !strcmp(which,"sync-close") && S_ISREG(st.st_mode)) {
        sync_fault=true;faulted=true;errno=ENOSPC;return -1;
    }
    if (!faulted && !strcmp(which,"file-sync") && S_ISREG(st.st_mode)) {
        faulted=true;errno=EIO;return -1;
    }
    if (!faulted && !strcmp(which,"dir-sync") && fd==etc_fd && fstab_renamed) {
        faulted=true;errno=EIO;return -1;
    }
    return __real_fsync(fd);
}
int __wrap_close(int fd)
{
    struct stat st;bool match=!fstat(fd,&st) && S_ISREG(st.st_mode);
    int ret=__real_close(fd);
    if (!close_fault && !strcmp(which,"sync-close") && match && sync_fault) {
        close_fault=true;errno=EIO;return -1;
    }
    if (!faulted && !strcmp(which,"file-close") && match) {
        faulted=true;errno=EIO;return -1;
    }
    if (!faulted && !strcmp(which,"dir-close") && fd==etc_fd && fstab_renamed) {
        faulted=true;errno=EIO;return -1;
    }
    return ret;
}
static void contents(const char *path,const char *expected)
{
    char text[1024];int fd=open(path,O_RDONLY);assert(fd>=0);
    ssize_t n=read(fd,text,sizeof(text)-1);assert(n>=0 && !close(fd));text[n]=0;
    assert(strstr(text,expected));
}
int main(int argc,char **argv)
{
    assert(argc==3);which=argv[1];
    if (!installer_swap_configure) {fputs("FAIL: fresh swap configuration transaction absent\n",stderr);return 1;}
    bool enabled=strcmp(which,"disabled")!=0, committed=false;
    int rc=installer_swap_configure(argv[2],"root-id","esp-id",enabled,&committed);
    char path[1024];
    snprintf(path,sizeof(path),"%s/etc/fstab",argv[2]);
    bool success=!strcmp(which,"enabled") || !strcmp(which,"disabled");
    bool published=success || !strcmp(which,"backup-unlink") || !strcmp(which,"dir-close");
    if (published) {
        assert(committed && (success ? !rc : rc<0 && faulted));contents(path,"/dev/disk/by-partuuid/root-id");
        contents(path,"/dev/disk/by-partuuid/esp-id /efi vfat defaults 0 2");
        contents(path,"/efi/reliefos/instances/root-id /boot none bind 0 0");
        if (enabled) contents(path,"/swapfile none swap sw 0 0");
    } else {assert(rc<0 && !committed && faulted);if (!strcmp(which,"sync-close")) assert(rc==-ENOSPC && close_fault);
            contents(path,"old fstab");}
    snprintf(path,sizeof(path),"%s/etc/conf.d/swap",argv[2]);
    contents(path,published && enabled ? "rc_need=\"localmount\"" : "original conf");
    snprintf(path,sizeof(path),"%s/etc/runlevels/default/swap",argv[2]);
    assert((access(path,F_OK)==0)==(published && enabled));
    snprintf(path,sizeof(path),"%s/etc/runlevels/default/reliefos-swap-check",argv[2]);
    assert((access(path,F_OK)==0)==(published && enabled));
    printf("PASS %s: fstab and swap activation transaction\n",which);return 0;
}
