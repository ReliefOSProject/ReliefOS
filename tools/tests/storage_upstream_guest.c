/* Target musl reference probe; writes only disposable guest files/mounts. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;
unsigned test_tmpfs_mmap(const char *directory);
unsigned test_tmpfs_mmap_full(int fd);
unsigned test_tmpfs_mount_mappings(const char *directory);
static void check(int ok, const char *label)
{
    printf("[storage-upstream] %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static int run_tool(const char *label, char *const argv[], const char *input, const char *expected)
{
    puts("[storage-upstream] BEGIN");
    printf("[storage-upstream] command=%s\n", label);
    int in = open("/tmp/storage-test-input", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (in < 0) { check(0, "open input"); return -1; }
    if (input) write(in, input, strlen(input));
    lseek(in, 0, SEEK_SET);
    pid_t child = fork();
    if (child == 0) {
        int output = open("/tmp/storage-test-output", O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output < 0 || dup2(in, 0) < 0 || dup2(output, 1) < 0 || dup2(output, 2) < 0) _exit(126);
        close(in); close(output);
        execv(argv[0], argv);
        perror("exec official tool");
        _exit(127);
    }
    close(in);
    int status = 0, ready = 0;
    for (unsigned n = 0; child > 0 && n < 200; ++n) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) { ready = 1; break; }
        if (result < 0 && errno != EINTR) break;
        usleep(100000);
    }
    if (child > 0 && !ready) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    char buffer[8192];
    int output = open("/tmp/storage-test-output", O_RDONLY);
    ssize_t length = output < 0 ? -1 : read(output, buffer, sizeof(buffer) - 1);
    if (output >= 0) close(output);
    if (length < 0) length = 0;
    buffer[length] = 0;
    printf("%s\n[storage-upstream] status=%d ready=%d\n", buffer, status, ready);
    int ok = ready && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             (!expected || strstr(buffer, expected));
    check(ok, label);
    return ok ? 0 : -1;
}
static int make_file(const char *path, off_t size)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    int ok = fd >= 0 && ftruncate(fd, size) == 0;
    if (fd >= 0) close(fd);
    check(ok, "create disposable guest image");
    return ok;
}
static void check_transfers(const char *path)
{
    size_t length=1<<20;
    unsigned char *data=malloc(length),*result=malloc(length);
    if (!data || !result) { check(0,"allocate transfer buffers"); free(data); free(result); return; }
    for (size_t i=0;i<length;++i) data[i]=(i*37)^(i>>8);
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);
    check(fd>=0 && write(fd,data,length)==(ssize_t)length,"1 MiB write completed");
    check(lseek(fd,0,SEEK_SET)==0 && read(fd,result,length)==(ssize_t)length &&
          !memcmp(data,result,length),"1 MiB read content");
    check(read(fd,result,1)==0,"EOF after complete transfer");
    memset(result,0,length);
    check(pwrite(fd,data,length,123)==(ssize_t)length && lseek(fd,0,SEEK_CUR)==(off_t)length &&
          pread(fd,result,length,123)==(ssize_t)length && !memcmp(data,result,length) &&
          lseek(fd,0,SEEK_CUR)==(off_t)length,"positional transfer preserves offset");
    int other=dup(fd);
    check(lseek(other,17,SEEK_SET)==17 && read(fd,result,31)==31 &&
          lseek(other,0,SEEK_CUR)==48,"duplicated descriptor shares offset");
    close(other);
    unlink(path);
    check(pread(fd,result,length,123)==(ssize_t)length && !memcmp(data,result,length),"unlinked open file retains content");
    close(fd); free(data); free(result);
}
static void check_tmpfs(void)
{
    const char *dir="/tmp/storage-mount";
    struct statfs space;
    if (statfs(dir,&space)!=0 || space.f_type!=0x01021994) { check(0,"real tmpfs superblock"); return; }
    check_transfers("/tmp/storage-mount/large");
    int fd=open("/tmp/storage-mount/held",O_CREAT|O_RDWR,0600);
    check(fd>=0 && write(fd,"held",4)==4,"tmpfs held file");
    check(umount(dir)<0 && errno==EBUSY,"tmpfs open descriptor blocks unmount");
    check(mkdir("/tmp/storage-mount/sub",0700)==0 &&
          rename("/tmp/storage-mount/held","/tmp/storage-mount/sub/moved")==0 &&
          link("/tmp/storage-mount/sub/moved","/tmp/storage-mount/link")==0,"tmpfs cross-directory rename and hardlink");
    struct stat st;
    check(fchmod(fd,0640)==0 && fchown(fd,1234,2345)==0 && fstat(fd,&st)==0 &&
          st.st_uid==1234 && st.st_gid==2345 && (st.st_mode&07777)==0640 && st.st_nlink==2,"tmpfs native permissions and link count");
    pid_t child=fork();
    if (!child) {
        if (setgid(5432)!=0 || setuid(5432)!=0) _exit(2);
        int opened=open("/tmp/storage-mount/link",O_WRONLY);
        _exit(opened<0 && errno==EACCES ? 0 : 3);
    }
    int status=0;
    check(child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"tmpfs denied write by other user");
    check(unlink("/tmp/storage-mount/link")==0 && unlink("/tmp/storage-mount/sub/moved")==0 &&
          fstat(fd,&st)==0 && st.st_nlink==0,"tmpfs last unlink keeps open inode");
    close(fd);
    check(chdir(dir)==0 && umount(dir)<0 && errno==EBUSY && chdir("/")==0,"tmpfs working directory blocks unmount");
}
static void check_block_io(void)
{
    int fd=open("/dev/sda1",O_RDWR);
    unsigned char *data=malloc(1<<20), *got=malloc(1<<20);
    if (fd<0 || !data || !got) { check(0,"block transfer setup"); close(fd); free(data); free(got); return; }
    memset(data,0xa5,1<<20);
    check(pwrite(fd,data,1<<20,0)==1<<20 && pread(fd,got,1<<20,0)==1<<20 &&
          !memcmp(data,got,1<<20),"block 1 MiB positional transfer");
    memset(data+513,0x3b,5003);
    check(pwrite(fd,data+513,5003,513)==5003 && pread(fd,got,8192,0)==8192 &&
          !memcmp(data,got,8192),"block unaligned write preserves adjacent bytes");
    off_t end=lseek(fd,0,SEEK_END);
    check(end==(16<<20) && read(fd,got,1)==0 && write(fd,data,1)<0 && errno==ENOSPC,"block EOF and end-of-device ENOSPC");
    check(pwrite(fd,data,128,end-17)==17 && pread(fd,got,128,end-17)==17 &&
          !memcmp(data,got,17),"block boundary partial transfer");
    check(fsync(fd)==0 && fdatasync(fd)==0,"block fsync and fdatasync");
    close(fd); free(data); free(got);
}
static void check_tmpfs_limits(void)
{
    const char *dir="/tmp/storage-mount";
    if (mount("tmpfs",dir,"tmpfs",0,"size=8k,nr_inodes=3,mode=1770")<0) { check(0,"mount bounded tmpfs"); return; }
    char data[16384]; memset(data,0x5a,sizeof(data));
    int fd=open("/tmp/storage-mount/full",O_CREAT|O_RDWR,0600);
    check(fd>=0 && write(fd,data,sizeof(data))==8192 && write(fd,data,1)<0 && errno==ENOSPC,"tmpfs capacity and partial ENOSPC write");
    struct statfs space;
    check(fstatfs(fd,&space)==0 && space.f_blocks==2 && space.f_bfree==0,"tmpfs accurate capacity accounting");
    failures += test_tmpfs_mmap_full(fd);
    check(ftruncate(fd,17)==0 && ftruncate(fd,8192)==0,"tmpfs sparse truncate");
    memset(data,0xff,sizeof(data));
    int ok=pread(fd,data,sizeof(data),0)==8192;
    for (unsigned i=17;i<8192;++i) if (data[i]) ok=0;
    check(ok,"tmpfs truncated bytes never reappear");
    int second=open("/tmp/storage-mount/second",O_CREAT|O_RDWR,0600);
    int third=open("/tmp/storage-mount/third",O_CREAT|O_RDWR,0600);
    check(second>=0 && third<0 && errno==ENOSPC,"tmpfs inode limit enforced");
    if (third>=0) close(third);
    close(second); close(fd);
    check(mount(NULL,dir,NULL,MS_REMOUNT|MS_RDONLY,NULL)==0,"tmpfs readonly remount");
    fd=open("/tmp/storage-mount/full",O_WRONLY);
    check(fd<0 && errno==EROFS,"tmpfs readonly open rejected");
    if(fd>=0) close(fd);
    check(unlink("/tmp/storage-mount/full")<0 && errno==EROFS,"tmpfs readonly unlink rejected");
    check(umount(dir)==0,"bounded tmpfs unmount");
    check(access("/tmp/storage-mount/full",F_OK)<0 && errno==ENOENT,"tmpfs unmount restores underlying directory");
}
#define RUN(label, input, expected, ...) do { \
    char *args[] = {__VA_ARGS__, NULL}; run_tool(label, args, input, expected); \
} while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("LC_ALL", "C", 1);
    puts("[storage-upstream] START");
#ifdef PROBE_POWER_COMMAND
    /* The inventory slot starts concurrently with init's ELF/runtime startup.
     * Exercise commands after init has installed its power-signal mask. */
    int init_ready = 0;
    unsigned long long wanted = (1ULL << (SIGUSR1 - 1)) |
                                (1ULL << (SIGUSR2 - 1)) |
                                (1ULL << (SIGTERM - 1));
    for (unsigned attempt = 0; attempt < 200 && !init_ready; ++attempt) {
        FILE *status = fopen("/proc/1/stat", "r");
        if (status) {
            char line[2048];
            if (fgets(line, sizeof(line), status)) {
                char *end = strrchr(line, ')'), *save = NULL;
                char *field = end ? strtok_r(end + 1, " ", &save) : NULL;
                for (unsigned number = 3; field && number < 32; ++number)
                    field = strtok_r(NULL, " ", &save);
                if (field && (strtoull(field, NULL, 10) & wanted) == wanted)
                    init_ready = 1;
            }
            fclose(status);
        }
        if (!init_ready) usleep(100000);
    }
    if (!init_ready) {
        check(0, "init power signal mask ready");
        return 1;
    }
    printf("[storage-upstream] upstream BusyBox %s request\n", PROBE_POWER_COMMAND);
    execl("/bin/busybox", "busybox", PROBE_POWER_COMMAND, (char *)NULL);
    perror("exec BusyBox power command");
    return 1;
#endif
    check_transfers("/tmp/storage-transfer");
    RUN("BusyBox pipeline", NULL, "3", "/bin/busybox", "sh", "-c", "printf abc | wc -c");
    RUN("file ELF and text without magic warnings", NULL, "FILE_OK", "/bin/sh", "-c",
        "set -e; printf 'Hello from ReliefOS\\n' >/tmp/file-text.txt; "
        "/usr/bin/file /bin/busybox /tmp/file-text.txt >/tmp/file-result.txt 2>/tmp/file-errors.txt; "
        "cat /tmp/file-result.txt; cat /tmp/file-errors.txt; "
        "test ! -s /tmp/file-errors.txt; "
        "grep -q 'ELF 64-bit' /tmp/file-result.txt; "
        "grep -q 'ASCII text' /tmp/file-result.txt; echo FILE_OK");
    RUN("file missing input rejected", NULL, "FILE_MISSING_OK", "/bin/sh", "-c",
        "if /usr/bin/file -E /tmp/absent-file-input; then exit 1; "
        "else echo FILE_MISSING_OK; fi");
    RUN("boot payload copy", NULL, "COPY_OK", "/bin/sh", "-c",
        "set -e; s=/tmp/storage-copy-source; d='/tmp/storage copy target'; "
        "mkdir -p \"$s/EFI/BOOT\" \"$s/reliefos\" \"$s/leonos\" \"$s/grub/fonts\" \"$d\"; "
        "for f in EFI/BOOT/BOOTX64.EFI reliefos/kernel.sys leonos/kernel.sys grub/grub.cfg grub/fonts/test.pf2; "
        "do printf '%s' \"$f\" > \"$s/$f\"; done; "
        "/usr/sbin/leonos-grub-installer --source \"$s\" \"$d\"; "
        "for f in EFI/BOOT/BOOTX64.EFI reliefos/kernel.sys leonos/kernel.sys grub/grub.cfg grub/fonts/test.pf2; "
        "do test \"$(cat \"$d/$f\")\" = \"$f\"; done; echo COPY_OK");
    RUN("boot missing payload rejected", NULL, "REJECT_OK", "/bin/sh", "-c",
        "if /usr/sbin/leonos-grub-installer --source /tmp/missing-boot-payload '/tmp/storage copy target'; "
        "then exit 1; else echo REJECT_OK; fi");
    RUN("fdisk version", NULL, "util-linux 2.42.4", "/usr/sbin/fdisk", "--version");
    RUN("mount version", NULL, "util-linux 2.42.4", "/bin/mount", "--version");
    RUN("mount listing", NULL, " on /", "/bin/mount");
    RUN("lsblk inventory", NULL, "blockdevices", "/bin/lsblk", "--json", "--output", "NAME,TYPE");
    RUN("lsblk actual disk and partition",NULL,"sda1","/bin/lsblk","--bytes","--output","NAME,TYPE,SIZE,MAJ:MIN");
    struct stat disk_stat;
    check(stat("/dev/sda1",&disk_stat)==0 && S_ISBLK(disk_stat.st_mode) &&
          major(disk_stat.st_rdev)==259 && minor(disk_stat.st_rdev)==1,"block stat device number matches sysfs");
    RUN("fsck dispatcher", NULL, "fsck.ext4", "/usr/sbin/fsck", "-N", "-t", "ext4", "/tmp/storage-test.img");
    if (make_file("/tmp/storage-test.img", 64 << 20)) {
        RUN("fdisk GPT write", "g\nn\n1\n\n+16M\nw\n", NULL, "/usr/sbin/fdisk", "/tmp/storage-test.img");
        RUN("fdisk GPT read", NULL, "gpt", "/usr/sbin/fdisk", "-l", "/tmp/storage-test.img");
    }
    if (make_file("/tmp/storage-test.img", 64 << 20)) {
        RUN("mkfs ext4", NULL, NULL, "/usr/sbin/mkfs.ext4", "-F", "/tmp/storage-test.img");
        RUN("fsck ext4", NULL, NULL, "/usr/sbin/fsck.ext4", "-n", "/tmp/storage-test.img");
        RUN("blkid ext4", NULL, "ext4", "/usr/sbin/blkid", "-p", "-o", "value", "-s", "TYPE", "/tmp/storage-test.img");
    }
    if (make_file("/tmp/storage-test.img", 64 << 20)) {
        RUN("mkfs ext2", NULL, NULL, "/usr/sbin/mkfs.ext2", "-F", "/tmp/storage-test.img");
        RUN("fsck ext2", NULL, NULL, "/usr/sbin/fsck.ext2", "-n", "/tmp/storage-test.img");
        RUN("blkid ext2", NULL, "ext2", "/usr/sbin/blkid", "-p", "-o", "value", "-s", "TYPE", "/tmp/storage-test.img");
    }
    if (make_file("/tmp/storage-test.img", 64 << 20)) {
        RUN("mkfs FAT32", NULL, NULL, "/usr/sbin/mkfs.fat", "-F", "32", "/tmp/storage-test.img");
        RUN("fsck FAT32", NULL, NULL, "/usr/sbin/fsck.fat", "-n", "/tmp/storage-test.img");
        RUN("blkid FAT32", NULL, "vfat", "/usr/sbin/blkid", "-p", "-o", "value", "-s", "TYPE", "/tmp/storage-test.img");
    }
    if (make_file("/tmp/storage-test.img", 64 << 20)) {
        RUN("mkfs exFAT", NULL, NULL, "/usr/sbin/mkfs.exfat", "/tmp/storage-test.img");
        RUN("fsck exFAT", NULL, NULL, "/usr/sbin/fsck.exfat", "-n", "/tmp/storage-test.img");
        RUN("blkid exFAT", NULL, "exfat", "/usr/sbin/blkid", "-p", "-o", "value", "-s", "TYPE", "/tmp/storage-test.img");
    }
    unlink("/tmp/storage-test.img");
    check_block_io();
    RUN("mkfs ext4 on AHCI partition",NULL,NULL,"/usr/sbin/mkfs.ext4","-F","/dev/sda1");
    RUN("fsck ext4 on AHCI partition",NULL,NULL,"/usr/sbin/fsck.ext4","-n","/dev/sda1");
    mkdir("/tmp/storage-mount", 0700);
    RUN("mount tmpfs", NULL, NULL, "/bin/mount", "-t", "tmpfs", "tmpfs", "/tmp/storage-mount");
    check_tmpfs();
    failures += test_tmpfs_mmap("/tmp/storage-mount");
    failures += test_tmpfs_mount_mappings("/tmp/storage-mount");
    RUN("BusyBox tmpfs tools",NULL,"TMPFS_TOOLS_OK","/bin/busybox","sh","-c",
        "set -e; cd /tmp/storage-mount; printf 'b\\na\\na\\n' > data; "
        "cp data copy; cmp data copy; ln -s copy symbolic; cmp data symbolic; "
        "mkdir output; mv copy output/moved; tar czf files.tgz data output; "
        "rm -r output; tar xzf files.tgz; cmp data output/moved; "
        "test $(sort data | uniq | wc -l) = 2; echo TMPFS_TOOLS_OK");
    RUN("umount tmpfs", NULL, NULL, "/bin/umount", "/tmp/storage-mount");
    check_tmpfs_limits();
    printf("[storage-upstream] DONE failures=%u\n", failures);
    return failures ? 1 : 0;
}
