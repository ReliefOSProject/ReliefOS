#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../userland/apps/installer/installer_swap.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_renameat(int,const char *,int,const char *);
int __real_unlinkat(int,const char *,int);
int __real_fsync(int);
int __real_fstat(int,struct stat *);
int __real_linkat(int,const char *,int,const char *,int);
int __real_close(int);
static const char *scenario;
static bool fault;
static bool fstab_published, rollback_fault, close_fault;
static struct stat etc_identity,root_identity;
int __wrap_fchown(int fd,uid_t uid,gid_t gid)
{ (void)fd;assert(!uid && !gid);return 0; }
int __wrap_fstat(int fd,struct stat *st)
{
    int rc=__real_fstat(fd,st);
    /* Model root ownership; the fixture itself runs as an unprivileged user. */
    if (!rc && st->st_dev==root_identity.st_dev && st->st_ino==root_identity.st_ino) st->st_uid=0;
    return rc;
}
int __wrap_linkat(int oldfd,const char *old,int newfd,const char *name,int flags)
{
    if ((flags & AT_EMPTY_PATH) && !*old) {
        char proc[64];assert(snprintf(proc,sizeof(proc),"/proc/self/fd/%d",oldfd)<(int)sizeof(proc));
        return __real_linkat(AT_FDCWD,proc,newfd,name,AT_SYMLINK_FOLLOW);
    }
    return __real_linkat(oldfd,old,newfd,name,flags);
}
int __wrap_renameat(int from,const char *a,int to,const char *b)
{
    if (!fault && (!strcmp(scenario,"fstab-failure") || !strcmp(scenario,"preexisting-reference")) && !strcmp(b,"fstab")) {
        fault=true;errno=EIO;return -1;
    }
    if (!strcmp(scenario,"rollback-failure") && !strcmp(a,".reliefos-fstab.backup") &&
        !strcmp(b,"fstab")) {rollback_fault=true;errno=EACCES;return -1;}
    int result=__real_renameat(from,a,to,b);
    if (!result && !strcmp(b,"fstab")) fstab_published=true;
    return result;
}
int __wrap_fsync(int fd)
{
    struct stat st;assert(!fstat(fd,&st));
    if (!fault && (!strcmp(scenario,"rollback-failure") || !strcmp(scenario,"rollback-restored") ||
                   !strcmp(scenario,"rollback-close")) && fstab_published &&
        st.st_dev==etc_identity.st_dev && st.st_ino==etc_identity.st_ino) {
        fault=true;errno=!strcmp(scenario,"rollback-close") ? ENOSPC : EIO;return -1;
    }
    return __real_fsync(fd);
}
int __wrap_close(int fd)
{
    struct stat st;
    bool etc=!fstat(fd,&st) && st.st_dev==etc_identity.st_dev && st.st_ino==etc_identity.st_ino;
    int rc=__real_close(fd);
    if (etc && fault && !close_fault && !strcmp(scenario,"rollback-close")) {
        close_fault=true;errno=EIO;return -1;
    }
    return rc;
}
int __wrap_unlinkat(int dir,const char *name,int flags)
{
    if (!fault && !strcmp(scenario,"post-commit-failure") && !strcmp(name,".reliefos-fstab.backup")) {
        fault=true;errno=EIO;return -1;
    }
    return __real_unlinkat(dir,name,flags);
}
static void expect_text(const char *path,const char *needle)
{
    char data[1024];int fd=open(path,O_RDONLY);assert(fd>=0);
    ssize_t n=read(fd,data,sizeof(data)-1);assert(n>=0 && !close(fd));data[n]=0;
    assert(strstr(data,needle));
}
int main(int argc,char **argv)
{
    assert(argc==3);scenario=argv[1];const char *root=argv[2];
    bool disabled=!strcmp(scenario,"disabled"), success=!strcmp(scenario,"success") || disabled;
    bool committed=success || !strcmp(scenario,"post-commit-failure");
    char swap[1024],fstab[1024],conf[1024],link[1024];
    assert(snprintf(swap,sizeof(swap),"%s/swapfile",root)<(int)sizeof(swap));
    assert(snprintf(fstab,sizeof(fstab),"%s/etc/fstab",root)<(int)sizeof(fstab));
    assert(snprintf(conf,sizeof(conf),"%s/etc/conf.d/swap",root)<(int)sizeof(conf));
    assert(snprintf(link,sizeof(link),"%s/etc/runlevels/default/reliefos-swap-check",root)<(int)sizeof(link));
    char etc[1024];assert(snprintf(etc,sizeof(etc),"%s/etc",root)<(int)sizeof(etc));
    assert(!stat(etc,&etc_identity) && !stat(root,&root_identity));
    int result=installer_swap_prepare(root,!strcmp(scenario,"bad-uuid") ? "" : "root-id",
                                      "esp-id",disabled ? 0 : 1,NULL,NULL);
    int loader_calls=0;if (!result) ++loader_calls;
    assert((result==0)==success && loader_calls==(int)success);
    if (committed) {
        expect_text(fstab,"/dev/disk/by-partuuid/root-id");
        if (!disabled) {
            expect_text(fstab,"/swapfile none swap sw 0 0");
            expect_text(conf,"rc_need=\"localmount\"");assert(!access(link,F_OK));
            struct stat st;assert(!stat(swap,&st) && st.st_size==1048576 && (st.st_mode&0777)==0600);
            char sig[10];int fd=open(swap,O_RDONLY);assert(fd>=0);
            assert(pread(fd,sig,10,4086)==10 && !memcmp(sig,"SWAPSPACE2",10) && !close(fd));
        } else {assert(access(swap,F_OK)<0 && errno==ENOENT);expect_text(conf,"original conf");}
    } else if (!strcmp(scenario,"rollback-failure")) {
        assert(result==-EIO && fault && rollback_fault);
        expect_text(fstab,"/swapfile none swap sw 0 0");
        if (access(swap,F_OK)<0) {
            fputs("FAIL: rollback failure left fstab referencing a removed swapfile\n",stderr);return 1;
        }
        expect_text(conf,"original conf");
        assert(access(link,F_OK)<0 && errno==ENOENT);
    } else if (!strcmp(scenario,"rollback-close")) {
        assert(result==-ENOSPC && fault && close_fault);
        expect_text(fstab,"old fstab");expect_text(conf,"original conf");
        if (access(swap,F_OK)<0) {
            fputs("FAIL: directory close failure left rollback durability uncertain but removed swapfile\n",stderr);return 1;
        }
    } else if (!strcmp(scenario,"preexisting-reference") || !strcmp(scenario,"bad-uuid")) {
        assert(result==(!strcmp(scenario,"bad-uuid") ? -EINVAL : -EIO));
        expect_text(fstab,"/swapfile none swap sw 0 0");
        assert(!access(swap,F_OK));
        expect_text(conf,"original conf");
        assert(access(link,F_OK)<0 && errno==ENOENT);
    } else {
        expect_text(fstab,"old fstab");expect_text(conf,"original conf");
        assert(access(link,F_OK)<0 && errno==ENOENT);
        if (!strcmp(scenario,"existing")) expect_text(swap,"keep");
        else assert(access(swap,F_OK)<0 && errno==ENOENT);
    }
    printf("PASS %s: real mkswap, fstab policy and loader gate\n",scenario);return 0;
}
