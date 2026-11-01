/* Real file publication with controlled short writes and external mkswap. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../userland/apps/installer/installer_swap.h"

typedef void (*progress_fn)(uint64_t,uint64_t,void *);
extern int installer_swap_create(const char *,uint64_t,progress_fn,void *) __attribute__((weak));
int __real_openat(int,const char *,int,...);
int __real_open(const char *,int,...);
int __real_close(int);
ssize_t __real_write(int,const void *,size_t);
int __real_fsync(int);
int __real_fstat(int,struct stat *);
int __real_linkat(int,const char *,int,const char *,int);
int __real_unlinkat(int,const char *,int);

static const char *scenario;
static int swap_fd=-1, sync_calls, write_calls, progress_calls, wait_calls;
static int root_fd=-1, close_fault_done, unlink_fault_done;
static uint64_t delivered;

int __wrap_openat(int dirfd,const char *name,int flags,...)
{
    mode_t mode=0;
    if (flags & O_CREAT) { va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap); }
    int fd=__real_openat(dirfd,name,flags,mode);
    if (fd>=0 && !strcmp(name,".reliefos-swap.tmp")) swap_fd=fd;
    return fd;
}
ssize_t __wrap_write(int fd,const void *buf,size_t n)
{
    if (fd==swap_fd) {
        ++write_calls;
        if (!strcmp(scenario,"eintr-short") && write_calls==1) { errno=EINTR;return -1; }
        if ((!strcmp(scenario,"enospc") || !strcmp(scenario,"cleanup-fail") ||
             !strcmp(scenario,"write-close-file") || !strcmp(scenario,"write-close-root")) && delivered>=262144)
            { errno=ENOSPC;return -1; }
        if (!strcmp(scenario,"eintr-short") && n>4096) n=4096;
        ssize_t result=__real_write(fd,buf,n);
        if (result>0) delivered+=(uint64_t)result;
        return result;
    }
    return __real_write(fd,buf,n);
}
int __wrap_unlinkat(int dirfd,const char *name,int flags)
{
    if ((!strcmp(scenario,"unlink-temp") && !unlink_fault_done && !strcmp(name,".reliefos-swap.tmp")) ||
        (!strcmp(scenario,"cleanup-fail") && !strcmp(name,".reliefos-swap.tmp"))) {
        unlink_fault_done=1;errno=EIO;return -1;
    }
    int rc=__real_unlinkat(dirfd,name,flags);
    if (!rc && !strcmp(scenario,"replace-final-after-unlink") && !strcmp(name,".reliefos-swap.tmp")) {
        assert(!renameat(root_fd,"swapfile",root_fd,"saved-original"));
        assert(!symlinkat("replacement-target",root_fd,"swapfile"));
    }
    return rc;
}
int __wrap_fchown(int fd,uid_t uid,gid_t gid)
{ assert(fd==swap_fd && uid==0 && gid==0);return 0; }
/* Model the target's root ownership without changing host ownership. */
int __wrap_fstat(int fd,struct stat *st)
{
    int rc=__real_fstat(fd,st);
    if (!rc && fd==root_fd) st->st_uid=!strcmp(scenario,"unsafe-root-owner") ? 12345 : 0;
    return rc;
}
int __wrap_fsync(int fd)
{
    ++sync_calls;
    if ((!strcmp(scenario,"sync-before") && sync_calls==1) ||
        (!strcmp(scenario,"sync-after") && sync_calls==2) ||
        (!strcmp(scenario,"sync-dir") && sync_calls==3)) { errno=EIO;return -1; }
    return __real_fsync(fd);
}
int __wrap_linkat(int oldfd,const char *old,int newfd,const char *newname,int flags)
{
    if (!strcmp(scenario,"publish")) { errno=EIO;return -1; }
    bool before=!strcmp(scenario,"replace-before-link");
    bool after=!strcmp(scenario,"replace-after-link");
    bool final=!strcmp(scenario,"replace-final-after-link");
    if (before) {
        assert(!renameat(root_fd,".reliefos-swap.tmp",root_fd,"saved-original"));
        assert(!symlinkat("replacement-target",root_fd,".reliefos-swap.tmp"));
    }
    int rc;
    if ((flags & AT_EMPTY_PATH) && !*old) {
        /* The host fixture lacks CAP_DAC_READ_SEARCH; this follows its open fd
         * to the same inode while production uses the native AT_EMPTY_PATH. */
        char proc[64];assert(snprintf(proc,sizeof(proc),"/proc/self/fd/%d",oldfd)<(int)sizeof(proc));
        rc=__real_linkat(AT_FDCWD,proc,newfd,newname,AT_SYMLINK_FOLLOW);
    } else rc=__real_linkat(oldfd,old,newfd,newname,flags);
    if (!rc && (after || final)) {
        const char *name=after ? ".reliefos-swap.tmp" : "swapfile";
        assert(!renameat(root_fd,name,root_fd,"saved-original"));
        assert(!symlinkat("replacement-target",root_fd,name));
    }
    return rc;
}
int __wrap_posix_spawnp(pid_t *pid,const char *file,const posix_spawn_file_actions_t *acts,
                        const posix_spawnattr_t *attrs,char *const argv[],char *const envp[])
{
    (void)envp;
    assert(!strcmp(file,"mkswap") && !acts && !attrs && argv && !strcmp(argv[0],"mkswap"));
    assert(argv[1] && !argv[2] && strstr(argv[1],"/.reliefos-swap.tmp"));
    if (!strcmp(scenario,"spawn")) return EIO;
    int fd=open(argv[1],O_WRONLY);assert(fd>=0);
    assert(pwrite(fd,"SWAPSPACE2",10,4086)==10 && !close(fd));
    *pid=123;return 0;
}
pid_t __wrap_waitpid(pid_t pid,int *status,int options)
{
    assert(pid==123 && !options);++wait_calls;
    if (!strcmp(scenario,"eintr-short") && wait_calls==1) {errno=EINTR;return -1;}
    *status=!strcmp(scenario,"mkswap") ? 1<<8 : 0;return pid;
}
static void progress(uint64_t done,uint64_t total,void *context)
{ assert(context==&progress_calls && done<=total && total==1048576);++progress_calls; }

int main(int argc,char **argv)
{
    assert(argc==3);scenario=argv[1];
    if (!installer_swap_create) { fputs("FAIL: transactional swapfile creation is absent\n",stderr);return 1; }
    char final[1024],tmp[1024];
    assert(snprintf(final,sizeof(final),"%s/swapfile",argv[2])<(int)sizeof(final));
    assert(snprintf(tmp,sizeof(tmp),"%s/.reliefos-swap.tmp",argv[2])<(int)sizeof(tmp));
    if (!strcmp(scenario,"unsafe-root-mode")) assert(!chmod(argv[2],0777));
    if (!strcmp(scenario,"existing")) {
        int fd=open(final,O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);
        assert(write(fd,"keep",4)==4 && !close(fd));
    }
    struct installer_swap_identity identity;
    bool owned=!strcmp(scenario,"owned-remove") || !strcmp(scenario,"owned-replaced");
    int result=owned ? installer_swap_create_owned(argv[2],1048576,progress,&progress_calls,&identity) :
                       installer_swap_create(argv[2],1048576,progress,&progress_calls);
    bool success=!strcmp(scenario,"success") || !strcmp(scenario,"eintr-short") || owned;
    if (success) {
        assert(!result && progress_calls && delivered==1048576);
        struct stat st;assert(!stat(final,&st) && st.st_size==1048576 && (st.st_mode&0777)==0600);
        int fd=open(final,O_RDONLY);assert(fd>=0);
        unsigned char data[4096];uint64_t offset=0;
        for (;;) {ssize_t count=read(fd,data,sizeof(data));assert(count>=0);if (!count)break;
            for (ssize_t i=0;i<count;++i,++offset) {
                unsigned char expected=offset>=4086 && offset<4096 ? (unsigned char)"SWAPSPACE2"[offset-4086] : 0;
                assert(data[i]==expected);
            }
        }
        assert(offset==1048576 && !close(fd));
        if (!strcmp(scenario,"owned-replaced")) {
            char previous[1024];assert(snprintf(previous,sizeof(previous),"%s/previous",argv[2])<(int)sizeof(previous));
            assert(!rename(final,previous));fd=open(final,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
            assert(write(fd,"keep",4)==4 && !close(fd));
            assert(installer_swap_remove_owned(argv[2],&identity)==-ESTALE);
            fd=open(final,O_RDONLY);assert(fd>=0);char keep[4];
            assert(read(fd,keep,4)==4 && !memcmp(keep,"keep",4) && !close(fd));
        } else if (owned) {
            assert(!installer_swap_remove_owned(argv[2],&identity));
            assert(access(final,F_OK)<0 && errno==ENOENT);
        }
    } else {
        if (result>=0) {
            fprintf(stderr,"FAIL %s: unsafe publication returned success\n",scenario);return 1;
        }
        if (!strcmp(scenario,"write-close-file") || !strcmp(scenario,"write-close-root"))
            assert(result==-ENOSPC && close_fault_done);
        if (!strcmp(scenario,"existing")) {
            char keep[4];int fd=open(final,O_RDONLY);assert(fd>=0);
            assert(read(fd,keep,4)==4 && !memcmp(keep,"keep",4) && !close(fd));
        } else if (!strcmp(scenario,"replace-final-after-link") || !strcmp(scenario,"replace-final-after-unlink")) {
            struct stat st;assert(!lstat(final,&st) && S_ISLNK(st.st_mode));
        } else {
            struct stat st;assert(lstat(final,&st)<0 && errno==ENOENT);
        }
    }
    if (!strcmp(scenario,"cleanup-fail")) assert(access(tmp,F_OK)==0);
    else if (!strcmp(scenario,"replace-before-link") || !strcmp(scenario,"replace-after-link")) {
        struct stat st;assert(!lstat(tmp,&st) && S_ISLNK(st.st_mode));
    }
    else assert(access(tmp,F_OK)<0 && errno==ENOENT);
    printf("PASS %s: swapfile creation and rollback\n",scenario);
    return 0;
}
int __wrap_open(const char *name,int flags,...)
{
    mode_t mode=0;
    if (flags & O_CREAT) { va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap); }
    int fd=__real_open(name,flags,mode);
    if (fd>=0 && (flags&O_DIRECTORY)) root_fd=fd;
    return fd;
}
int __wrap_close(int fd)
{
    int rc=__real_close(fd);
    if (!close_fault_done && ((!strcmp(scenario,"close-file") && fd==swap_fd) ||
                              (!strcmp(scenario,"close-root") && fd==root_fd) ||
                              (!strcmp(scenario,"write-close-file") && fd==swap_fd) ||
                              (!strcmp(scenario,"write-close-root") && fd==root_fd))) {
        close_fault_done=1;errno=EIO;return -1;
    }
    return rc;
}
