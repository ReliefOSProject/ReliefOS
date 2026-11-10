/* Fresh-target fstab and stock OpenRC configuration transaction. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "installer_swap.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct replacement {
    int dir;
    const char *name, *temp, *backup;
    struct stat old, staged;
    bool had_old, has_backup, has_temp, published;
};
struct new_link {const char *name, *target;struct stat inode;bool created;};

/** @brief Compare a directory entry without following a replacement symlink.
 * @param dir Borrowed directory fd. @param name Entry name. @param expected
 * Borrowed original stat identity. @return True only when device and inode match.
 */
static bool same_entry(int dir,const char *name,const struct stat *expected)
{
    struct stat actual;
    return !fstatat(dir,name,&actual,AT_SYMLINK_NOFOLLOW) &&
           actual.st_dev==expected->st_dev && actual.st_ino==expected->st_ino;
}

/** @brief Remove only an entry whose inode is owned by this transaction.
 * @param dir Borrowed directory fd. @param name Entry name. @param inode
 * Borrowed owned identity. Cleanup failures are reported to stderr.
 */
static void remove_owned(int dir,const char *name,const struct stat *inode)
{
    struct stat current;
    if (fstatat(dir,name,&current,AT_SYMLINK_NOFOLLOW)<0) {
        if (errno!=ENOENT) fprintf(stderr,"[installer.elf] configuration cleanup cannot inspect %s: %s\n",name,strerror(errno));
    } else if (current.st_dev!=inode->st_dev || current.st_ino!=inode->st_ino)
        fprintf(stderr,"[installer.elf] configuration cleanup left changed %s\n",name);
    else if (unlinkat(dir,name,0)<0)
        fprintf(stderr,"[installer.elf] configuration cleanup failed %s: %s\n",name,strerror(errno));
}

/** @brief Prepare a fully written, synced file and retain the original name.
 * @param f Mutable transaction record. @param text Complete NUL-terminated
 * contents. @return Zero or negative errno; the record tracks partial work.
 */
static int stage_file(struct replacement *f,const char *text)
{
    if (!fstatat(f->dir,f->name,&f->old,AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(f->old.st_mode)) return -EINVAL;
        f->had_old=true;
        if (linkat(f->dir,f->name,f->dir,f->backup,0)<0) return -errno;
        f->has_backup=true;
        if (!same_entry(f->dir,f->backup,&f->old)) return -EIO;
    } else if (errno!=ENOENT) return -errno;
    int fd=openat(f->dir,f->temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if (fd<0) return -errno;
    f->has_temp=true;
    int rc=0;
    if (fstat(fd,&f->staged)<0) rc=-errno;
    if (!rc && (fchown(fd,0,0)<0 || fchmod(fd,0644)<0)) rc=-errno;
    size_t length=strlen(text);
    for (size_t done=0;!rc && done<length;) {
        ssize_t n=write(fd,text+done,length-done);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) {rc=n<0 ? -errno : -EIO;break;}
        done+=(size_t)n;
    }
    if (!rc && fsync(fd)<0) rc=-errno;
    if (close(fd)<0) {
        int error=errno;
        fprintf(stderr,"[installer.elf] configuration file close failed: %s\n",strerror(error));
        if (!rc) rc=-error;
    }
    return rc;
}

/** @brief Replace the target only while the original and staged names match.
 * @param f Mutable transaction record. @return Zero or negative errno.
 */
static int publish_file(struct replacement *f)
{
    if (!same_entry(f->dir,f->temp,&f->staged)) return -ESTALE;
    if (f->had_old) {if (!same_entry(f->dir,f->name,&f->old)) return -ESTALE;}
    else {struct stat s;if (!fstatat(f->dir,f->name,&s,AT_SYMLINK_NOFOLLOW)) return -EEXIST;
          if (errno!=ENOENT) return -errno;}
    if (renameat(f->dir,f->temp,f->dir,f->name)<0) return -errno;
    f->has_temp=false;f->published=true;return 0;
}

/** @brief Restore an original file on a failed publication, if still owned.
 * @param f Mutable transaction record. Cleanup failures are reported to stderr.
 */
static void rollback_file(struct replacement *f)
{
    if (f->published) {
        if (!same_entry(f->dir,f->name,&f->staged))
            fprintf(stderr,"[installer.elf] configuration rollback left changed %s\n",f->name);
        else if (f->has_backup) {
            if (!same_entry(f->dir,f->backup,&f->old) ||
                renameat(f->dir,f->backup,f->dir,f->name)<0)
                fprintf(stderr,"[installer.elf] configuration rollback failed %s: %s\n",f->name,strerror(errno));
            else f->has_backup=false;
        } else remove_owned(f->dir,f->name,&f->staged);
    }
    if (f->has_temp) remove_owned(f->dir,f->temp,&f->staged);
    if (f->has_backup && !f->published) remove_owned(f->dir,f->backup,&f->old);
}

/** @brief Create or verify the exact standard service link, preserving existing links.
 * @param dir Borrowed runlevel directory fd. @param link Mutable link record.
 * @return Zero or negative errno; created links are recorded for rollback.
 */
static int prepare_link(int dir,struct new_link *link)
{
    if (symlinkat(link->target,dir,link->name)<0) {
        if (errno!=EEXIST) return -errno;
        char target[128];ssize_t n=readlinkat(dir,link->name,target,sizeof(target)-1);
        if (n<0) return -errno;
        target[n]=0;return strcmp(target,link->target) ? -EEXIST : 0;
    }
    link->created=true;
    return fstatat(dir,link->name,&link->inode,AT_SYMLINK_NOFOLLOW)<0 ? -errno : 0;
}

/** @brief Prove fstab has no active entries without interpreting user policy.
 * @param dir Borrowed etc directory fd. @return True only for an absent file
 * or an entirely blank/comment-only regular file read and closed successfully.
 * Any active, malformed, oversized or unreadable file remains uncertain.
 */
static bool fstab_has_no_entries(int dir)
{
    int fd=openat(dir,"fstab",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return errno==ENOENT;
    struct stat st;bool safe=true,line_start=true,comment=false;
    if (fstat(fd,&st)<0 || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>1024*1024) safe=false;
    char bytes[512];
    while (safe) {
        ssize_t n=read(fd,bytes,sizeof(bytes));
        if (n<0 && errno==EINTR) continue;
        if (n<0) {safe=false;break;}
        if (!n) break;
        for (ssize_t i=0;i<n;++i) {
            unsigned char c=(unsigned char)bytes[i];
            if (!c) {safe=false;break;}
            if (c=='\n') {line_start=true;comment=false;continue;}
            if (comment) continue;
            if (line_start && (c==' ' || c=='\t' || c=='\r')) continue;
            if (line_start && c=='#') {comment=true;line_start=false;continue;}
            safe=false;break;
        }
    }
    if (close(fd)<0) {
        fprintf(stderr,"[installer.elf] fstab inspection close failed: %s\n",strerror(errno));
        safe=false;
    }
    return safe;
}

/** @brief Publish fstab and optional target-only OpenRC ordering and health policy.
 * @param root Absolute freshly formatted target. @param root_uuid Root GPT UUID.
 * @param esp_uuid Boot GPT UUID. @param enabled Include the prepared swapfile.
 * @param retain_file Receives true after publication or if reference/rollback
 * durability is uncertain following an error. @return Zero or negative errno;
 * rollback is best effort and uncertain fstab state retains the swapfile.
 */
int installer_swap_configure(const char *root,const char *root_uuid,const char *esp_uuid,
                             bool enabled,bool *retain_file)
{
    if (retain_file) *retain_file=enabled;
    if (!root || root[0]!='/' || !strcmp(root,"/") || !root_uuid || !*root_uuid ||
        !esp_uuid || !*esp_uuid || !retain_file) return -EINVAL;
    char fstab[512];
    int n=snprintf(fstab,sizeof(fstab),
        "# <source> <mountpoint> <type> <options> <dump> <pass>\n"
        "/dev/disk/by-partuuid/%s / ext4 defaults 0 1\n"
        "/dev/disk/by-partuuid/%s /efi vfat defaults 0 2\n"
        "/efi/reliefos/instances/%s /boot none bind 0 0\n%s",
        root_uuid,esp_uuid,root_uuid,enabled ? "/swapfile none swap sw 0 0\n" : "");
    if (n<0 || (size_t)n>=sizeof(fstab)) return -EOVERFLOW;
    int rootfd=-1,etc=-1,conf=-1,levels=-1,run=-1,rc=0;
    bool published=false;
    struct replacement f={.dir=-1,.name="fstab",.temp=".reliefos-fstab.tmp",.backup=".reliefos-fstab.backup"};
    struct replacement c={.dir=-1,.name="swap",.temp=".reliefos-swap-conf.tmp",.backup=".reliefos-swap-conf.backup"};
    struct new_link links[]={ {"swap","../../init.d/swap",{0},false},
                              {"reliefos-swap-check","../../init.d/reliefos-swap-check",{0},false} };
    rootfd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (rootfd<0) return -errno;
    etc=openat(rootfd,"etc",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (etc<0) {rc=-errno;goto out;} f.dir=etc;
    if (enabled) {
        conf=openat(etc,"conf.d",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (conf<0) {rc=-errno;goto out;} c.dir=conf;
        levels=openat(etc,"runlevels",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (levels<0) {rc=-errno;goto out;}
        run=openat(levels,"default",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (run<0) {rc=-errno;goto out;}
        rc=stage_file(&c,"# Local swapfiles require mounted filesystems.\n"
                         "rc_before=\"!localmount\"\nrc_need=\"localmount\"\n");
        if (rc) goto out;
    }
    rc=stage_file(&f,fstab);
    if (rc) goto out;
    if (enabled) {
        rc=publish_file(&c);
        for (size_t i=0;!rc && i<sizeof(links)/sizeof(links[0]);++i) rc=prepare_link(run,&links[i]);
        if (!rc && fsync(conf)<0) rc=-errno;
        if (!rc && fsync(run)<0) rc=-errno;
        if (rc) goto out;
    }
    rc=publish_file(&f);
    if (!rc && fsync(etc)<0) rc=-errno;
    if (rc) goto out;
    /* Configuration is now complete. Failure removing backups or closing a
     * directory is still reported, but the caller must retain the swapfile. */
    published=true;*retain_file=true;
    if (f.has_backup) {
        if (!same_entry(etc,f.backup,&f.old) || unlinkat(etc,f.backup,0)<0) rc=-EIO;
        else f.has_backup=false;
    }
    if (c.has_backup) {
        if (!same_entry(conf,c.backup,&c.old) || unlinkat(conf,c.backup,0)<0) {if (!rc) rc=-EIO;}
        else c.has_backup=false;
    }
    if (fsync(etc)<0 && !rc) rc=-errno;
    if (enabled && fsync(conf)<0 && !rc) rc=-errno;
    if (rc) fprintf(stderr,"[installer.elf] configuration committed, cleanup/sync failed: %s\n",strerror(-rc));
out:
    if (rc && !published) {
        bool fstab_synced=etc>=0;
        if (f.dir>=0) rollback_file(&f);
        if (run>=0) for (size_t i=0;i<sizeof(links)/sizeof(links[0]);++i)
            if (links[i].created) remove_owned(run,links[i].name,&links[i].inode);
        if (c.dir>=0) rollback_file(&c);
        if (etc>=0 && fsync(etc)<0) {
            fprintf(stderr,"[installer.elf] fstab rollback sync failed: %s\n",strerror(errno));
            fstab_synced=false;
        }
        if (conf>=0 && fsync(conf)<0) fprintf(stderr,"[installer.elf] swap config rollback sync failed: %s\n",strerror(errno));
        if (run>=0 && fsync(run)<0) fprintf(stderr,"[installer.elf] service rollback sync failed: %s\n",strerror(errno));
        *retain_file=enabled && (!fstab_synced || !fstab_has_no_entries(etc));
    }
    int fds[]={run,levels,conf,etc,rootfd};
    for (size_t i=0;i<sizeof(fds)/sizeof(fds[0]);++i) if (fds[i]>=0 && close(fds[i])<0) {
        int error=errno;
        if (!rc) rc=-error;
        if (enabled) *retain_file=true;
        fprintf(stderr,"[installer.elf] configuration directory close failed: %s\n",strerror(error));
    }
    return rc;
}
