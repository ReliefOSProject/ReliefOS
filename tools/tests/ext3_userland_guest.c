/* SPDX-License-Identifier: Apache-2.0 */
/* Integration probe: the QEMU harness supplies only a new scratch disk. */
#include <errno.h>
#include <fcntl.h>
#include <reliefos/blockdev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Stop the private guest test with a diagnostic.
 * @param operation Failed stage. @param result Negative errno-style status.
 */
static void fail(const char *operation,int result)
{
    fprintf(stderr,"EXT3_USERLAND_FAIL %s result=%d errno=%d\n",operation,result,errno);
}

/** @brief Write or verify a deterministic file through the mounted filesystem.
 * @param verify Nonzero reads and checks an existing file; zero creates it.
 * @return Zero or negative errno.
 */
static int transfer(int verify)
{
    int fd=open("/mnt/ext3/check.bin",verify ? O_RDONLY : O_WRONLY|O_CREAT|O_TRUNC,0644);
    if (fd<0) return -errno;
    uint8_t bytes[4096]; int result=0;
    for (unsigned round=0;round<4 && !result;++round) {
        if (!verify) for (unsigned i=0;i<sizeof(bytes);++i) bytes[i]=(uint8_t)(i*37+round*19);
        size_t done=0;
        while (done<sizeof(bytes)) {
            ssize_t n=verify ? read(fd,bytes+done,sizeof(bytes)-done) : write(fd,bytes+done,sizeof(bytes)-done);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { result=n<0 ? -errno : -EIO; break; }
            done+=(size_t)n;
        }
        if (verify && !result) for (unsigned i=0;i<sizeof(bytes);++i)
            if (bytes[i]!=(uint8_t)(i*37+round*19)) { result=-EIO; break; }
    }
    if (!verify && !result && fsync(fd)<0) result=-errno;
    if (close(fd)<0 && !result) result=-errno;
    return result;
}

/** @brief Reload the ext3 module using the guest's installed modprobe.
 * @return Zero on successful reload or negative errno/EIO.
 */
static int reload_ext3(void)
{
    if (syscall(SYS_delete_module,"ext3",O_NONBLOCK|O_EXCL)<0) return -errno;
    pid_t child=fork();
    if (child<0) return -errno;
    if (!child) {
        char *const args[]={(char *)"modprobe",(char *)"ext3",NULL};
        execv("/bin/modprobe",args); _exit(127);
    }
    int status=0; pid_t waited;
    do { waited=waitpid(child,&status,0); } while (waited<0 && errno==EINTR);
    if (waited<0) return -errno;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
}

/** @brief Format, mount, persist and remount a partition on the owned scratch disk.
 * @param argc Argument count. @param argv Program, scratch device and optional module flag.
 * @return Zero only after all operations succeed.
 */
int main(int argc,char **argv)
{
    if ((argc!=2 && argc!=3) || strcmp(argv[1],"/dev/sda") ||
        (argc==3 && strcmp(argv[2],"--modular"))) { fail("scratch argument",-EINVAL); return 1; }
    int modular=argc==3;
    const char *disk=argv[1]; uint32_t index=0, filesystem=0; char path[RELIEFOS_BLOCK_PATH_LEN];
    int result=reliefos_block_gpt_initialize(disk,0);
    if (!result) result=reliefos_block_gpt_create(disk,RELIEFOS_BLOCK_FILESYSTEM_EXT3,128,"ext3-test",&index);
    if (!result) result=reliefos_block_partition_path(disk,index,path,sizeof(path));
    if (result) { fail("scratch partition",result); return 1; }
    result=reliefos_block_format(path,RELIEFOS_BLOCK_FILESYSTEM_EXT3,"EXT3QA");
    if (result) { fail("runtime format",result); return 1; }
    result=reliefos_block_probe_filesystem(path,&filesystem);
    if (result || filesystem!=RELIEFOS_BLOCK_FILESYSTEM_EXT3) {
        fail("runtime probe",result ? result : -EINVAL); return 1;
    }
    puts("EXT3_USERLAND_FORMAT_OK"); fflush(stdout);
    if ((mkdir("/mnt",0755)<0 && errno!=EEXIST) ||
        (mkdir("/mnt/ext3",0755)<0 && errno!=EEXIST)) { fail("mkdir",-errno); return 1; }
    if (mount(path,"/mnt/ext3","ext3",0,"data=ordered")<0) { fail("mount",-errno); return 1; }
    if (modular) {
        errno=0;
        if (syscall(SYS_delete_module,"ext3",O_NONBLOCK|O_EXCL)!=-1 || errno!=EWOULDBLOCK) {
            fail("mounted module not pinned",-errno); return 1;
        }
        puts("EXT3_USERLAND_PINNED_OK"); fflush(stdout);
    }
    result=transfer(0);
    int dir=open("/mnt/ext3",O_RDONLY|O_DIRECTORY);
    if (!result && dir<0) result=-errno;
    if (!result && fsync(dir)<0) result=-errno;
    if (dir>=0 && close(dir)<0 && !result) result=-errno;
    if (result) { fail("write",result); (void)umount2("/mnt/ext3",0); return 1; }
    if (mount(NULL,"/mnt/ext3",NULL,MS_REMOUNT|MS_RDONLY,"data=ordered")<0) {
        fail("ordered read-only remount",-errno); return 1;
    }
    result=transfer(1);
    errno=0;
    int forbidden=open("/mnt/ext3/forbidden",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if (forbidden>=0) { close(forbidden); result=-EIO; }
    else if (errno!=EROFS) result=-errno;
    if (result) { fail("read-only enforcement",result); return 1; }
    if (mount(NULL,"/mnt/ext3",NULL,MS_REMOUNT,"data=ordered")<0) {
        fail("ordered writable remount",-errno); return 1;
    }
    result=transfer(0);
    if (result) { fail("write after writable remount",result); return 1; }
    puts("EXT3_USERLAND_REMOUNT_OK"); fflush(stdout);
    if (umount2("/mnt/ext3",0)<0) { fail("unmount",-errno); return 1; }
    if (modular) {
        result=reload_ext3();
        if (result) { fail("module reload",result); return 1; }
        puts("EXT3_USERLAND_RELOAD_OK"); fflush(stdout);
    }
    if (mount(path,"/mnt/ext3","ext3",MS_RDONLY,"data=ordered")<0) { fail("remount",-errno); return 1; }
    result=transfer(1);
    if (umount2("/mnt/ext3",0)<0 && !result) result=-errno;
    if (result) { fail("read after remount",result); return 1; }
    puts("EXT3_USERLAND_IO_OK"); fflush(stdout); return 0;
}
