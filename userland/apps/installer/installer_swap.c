/* Fresh-install swapfile allocation; no shell or active target swap. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "installer_swap.h"
#include "installer_capacity.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static const unsigned char zeros[65536];

/* A caller must also keep the target mount and its ancestors private and
 * stable. Directory permissions do not exclude another privileged writer. */
static bool installer_swap_private_root(const struct stat *root)
{ return S_ISDIR(root->st_mode) && root->st_uid==0 && !(root->st_mode & 0022); }

/** @brief Confirm an observed name still denotes the retained regular file. */
static int installer_swap_verify_name(int root,const char *name,const struct stat *owned)
{
    struct stat current;
    if (fstatat(root,name,&current,AT_SYMLINK_NOFOLLOW)<0) {
        int error=errno;
        fprintf(stderr,"[installer.elf] swap path inspection failed for %s: %s\n",name,strerror(error));
        return -error;
    }
    if (!S_ISREG(current.st_mode) || current.st_dev!=owned->st_dev || current.st_ino!=owned->st_ino) {
        fprintf(stderr,"[installer.elf] swap path changed: %s\n",name);
        return -ESTALE;
    }
    return 0;
}

/** @brief Remove only a pathname still naming this invocation's file.
 * @param root Borrowed root directory fd. @param name Entry to remove.
 * @param owned Original identity, nullable when identity could not be read.
 * Errors are reported on stderr and never hide the initiating failure. The
 * exclusive-target precondition is required between inspection and unlink;
 * POSIX unlinkat cannot condition deletion on the previously observed inode.
 */
static void installer_swap_cleanup_name(int root,const char *name,const struct stat *owned)
{
    struct stat current;
    if (!owned || fstatat(root,name,&current,AT_SYMLINK_NOFOLLOW)<0) {
        if (!owned || errno!=ENOENT)
            fprintf(stderr,"[installer.elf] swap cleanup cannot inspect %s: %s\n",name,
                    owned ? strerror(errno) : "unknown file identity");
        return;
    }
    if (!S_ISREG(current.st_mode) || current.st_dev!=owned->st_dev || current.st_ino!=owned->st_ino) {
        fprintf(stderr,"[installer.elf] swap cleanup left changed path %s\n",name);return;
    }
    if (unlinkat(root,name,0)<0)
        fprintf(stderr,"[installer.elf] swap cleanup failed for %s: %s\n",name,strerror(errno));
}

/** @brief Run mkswap directly and report spawn, wait, or tool failure.
 * @param path Absolute unpublished file path. @return Zero or negative errno.
 */
static int installer_swap_format(const char *path)
{
    char *const argv[]={(char *)"mkswap",(char *)path,NULL};
    pid_t child;int status;
    int rc=posix_spawnp(&child,"mkswap",NULL,NULL,argv,environ);
    if (rc) return -rc;
    pid_t waited;
    do { waited=waitpid(child,&status,0); } while (waited<0 && errno==EINTR);
    if (waited<0) return -errno;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
}

/** @brief Fully allocate, format and publish a fresh swapfile without replacement.
 * @param target_root Absolute private target root. @param bytes Whole MiB size.
 * @param progress Optional synchronous callback. @param context Borrowed data.
 * @param identity Optional output set only on success. @return Zero or negative
 * errno; failed cleanup is logged and never replaces the original error.
 */
int installer_swap_create_owned(const char *target_root,uint64_t bytes,
                                installer_swap_progress_fn progress,void *context,
                                struct installer_swap_identity *identity)
{
    const char *temporary=".reliefos-swap.tmp", *final="swapfile";
    if (!target_root || target_root[0]!='/' || !strcmp(target_root,"/") ||
        !bytes || bytes%(1024u*1024u) || !installer_swap_size_valid(bytes/(1024u*1024u)))
        return -EINVAL;
    char path[1024];
    int length=snprintf(path,sizeof(path),"%s/%s",target_root,temporary);
    if (length<0 || (size_t)length>=sizeof(path)) return -ENAMETOOLONG;
    int root=open(target_root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (root<0) return -errno;
    int fd=-1, result=0;bool temp_owned=false, final_owned=false;
    struct stat state, root_identity;bool have_identity=false;
    if (fstat(root,&root_identity)<0) {result=-errno;goto out;}
    if (!installer_swap_private_root(&root_identity)) {
        fprintf(stderr,"[installer.elf] swap target is not a private root-owned directory\n");
        result=-EPERM;goto out;
    }
    if (!fstatat(root,final,&state,AT_SYMLINK_NOFOLLOW)) result=-EEXIST;
    else if (errno!=ENOENT) result=-errno;
    if (result) goto out;
    fd=openat(root,temporary,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) {result=-errno;goto out;}
    temp_owned=true;
    if (fstat(fd,&state)<0) {result=-errno;goto out;}
    have_identity=true;
    if (fchown(fd,0,0)<0 || fchmod(fd,0600)<0) {result=-errno;goto out;}
    for (uint64_t done=0;done<bytes;) {
        size_t want=bytes-done<sizeof(zeros) ? (size_t)(bytes-done) : sizeof(zeros);
        ssize_t n=write(fd,zeros,want);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) {result=n<0 ? -errno : -EIO;goto out;}
        done+=(uint64_t)n;
        if (progress) progress(done,bytes,context);
    }
    if (fsync(fd)<0) {result=-errno;goto out;}
    result=installer_swap_format(path);
    if (result) goto out;
    struct stat entry,formatted;
    if (fstat(fd,&formatted)<0 || fstatat(root,temporary,&entry,AT_SYMLINK_NOFOLLOW)<0) {
        result=-errno;goto out;
    }
    if (!S_ISREG(formatted.st_mode) || !S_ISREG(entry.st_mode) ||
        formatted.st_dev!=state.st_dev || formatted.st_ino!=state.st_ino ||
        state.st_dev!=entry.st_dev || state.st_ino!=entry.st_ino ||
        formatted.st_size<0 || (uint64_t)formatted.st_size!=bytes) {
        result=-EIO;goto out;
    }
    if (fsync(fd)<0) {result=-errno;goto out;}
    /* Linux AT_EMPTY_PATH publishes the retained file, while linkat refuses
     * an occupied destination. This requires CAP_DAC_READ_SEARCH. The fixed
     * exclusive temporary name serializes competing invocations on this root. */
    if (linkat(fd,"",root,final,AT_EMPTY_PATH)<0) {result=-errno;goto out;}
    final_owned=true;
    if ((result=installer_swap_verify_name(root,final,&state)) ||
        (result=installer_swap_verify_name(root,temporary,&state))) goto out;
    if (unlinkat(root,temporary,0)<0) {result=-errno;goto out;}
    temp_owned=false;
    if ((result=installer_swap_verify_name(root,final,&state))) goto out;
    if (fsync(root)<0) {result=-errno;goto out;}
out:
    if (fd>=0 && close(fd)<0) {
        int error=errno;
        fprintf(stderr,"[installer.elf] swap file close failed: %s\n",strerror(error));
        if (!result) result=-error;
    }
    if (result) {
        if (final_owned) installer_swap_cleanup_name(root,final,have_identity ? &state : NULL);
        if (temp_owned) installer_swap_cleanup_name(root,temporary,have_identity ? &state : NULL);
        if ((final_owned || temp_owned) && fsync(root)<0)
            fprintf(stderr,"[installer.elf] swap cleanup directory sync failed: %s\n",strerror(errno));
    }
    if (close(root)<0) {
        int error=errno;
        fprintf(stderr,"[installer.elf] swap target directory close failed: %s\n",strerror(error));
        if (!result) {
            result=-error;
            /* The descriptor may already be closed. Reopen only the same target
             * directory, and never remove a replacement inode on rollback. */
            int reopen=open(target_root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            struct stat reopened;
            if (reopen<0) fprintf(stderr,"[installer.elf] swap cleanup cannot reopen target: %s\n",strerror(errno));
            else {
                if (fstat(reopen,&reopened)<0 || reopened.st_dev!=root_identity.st_dev ||
                    reopened.st_ino!=root_identity.st_ino || !installer_swap_private_root(&reopened))
                    fprintf(stderr,"[installer.elf] swap cleanup target identity changed\n");
                else {
                    installer_swap_cleanup_name(reopen,final,have_identity ? &state : NULL);
                    if (fsync(reopen)<0)
                        fprintf(stderr,"[installer.elf] swap cleanup directory sync failed: %s\n",strerror(errno));
                }
                if (close(reopen)<0)
                    fprintf(stderr,"[installer.elf] reopened swap directory close failed: %s\n",strerror(errno));
            }
        }
    }
    if (!result && identity) *identity=(struct installer_swap_identity){
        root_identity.st_dev,state.st_dev,root_identity.st_ino,state.st_ino};
    return result;
}

/** @brief Prepare and publish a file without returning a rollback identity.
 * @param root Private target root. @param bytes Whole MiB size. @param progress
 * Optional callback. @param context Borrowed data. @return Zero or negative errno.
 */
int installer_swap_create(const char *root,uint64_t bytes,installer_swap_progress_fn progress,void *context)
{ return installer_swap_create_owned(root,bytes,progress,context,NULL); }

/** @brief Remove a published file only if its target and inode still match.
 * @param root Private target root. @param identity Identity from a successful
 * creation. @return Zero or negative inspection, unlink, sync or close errno.
 */
int installer_swap_remove_owned(const char *root,const struct installer_swap_identity *identity)
{
    if (!root || !identity || root[0]!='/' || !strcmp(root,"/")) return -EINVAL;
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -errno;
    struct stat dir,file;int rc=0;
    if (fstat(fd,&dir)<0 || fstatat(fd,"swapfile",&file,AT_SYMLINK_NOFOLLOW)<0) rc=-errno;
    else if (!installer_swap_private_root(&dir) ||
             dir.st_dev!=identity->root_dev || dir.st_ino!=identity->root_ino ||
             file.st_dev!=identity->file_dev || file.st_ino!=identity->file_ino ||
             !S_ISREG(file.st_mode)) rc=-ESTALE;
    else if (unlinkat(fd,"swapfile",0)<0) rc=-errno;
    else if (fsync(fd)<0) rc=-errno;
    if (close(fd)<0) {
        int error=errno;
        fprintf(stderr,"[installer.elf] swap removal directory close failed: %s\n",strerror(error));
        if (!rc) rc=-error;
    }
    return rc;
}
