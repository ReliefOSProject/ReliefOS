/* SPDX-License-Identifier: Apache-2.0 */
/* File-backed fixture for the real block-device probe and ext3 formatter. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <reliefos/blockdev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_exec;
int __real_execv(const char *path,char *const argv[]);
/** @brief Inject a missing formatter or run the real formatter.
 * @param path Executable path. @param argv Argument vector. @return Only on exec failure.
 */
int __wrap_execv(const char *path,char *const argv[])
{
    if (fail_exec) { errno=ENOENT; return -1; }
    return __real_execv(path,argv);
}

/** @brief Supply block geometry for a disposable regular image.
 * @param fd Open fixture. @param request Linux block ioctl. @param arg Output pointer.
 * @return Zero for supported requests, otherwise -1 with errno set.
 */
int __wrap_ioctl(int fd,unsigned long request,void *arg)
{
    struct stat st; if (fstat(fd,&st)<0) return -1;
    if (request==BLKGETSIZE64) *(uint64_t *)arg=(uint64_t)st.st_size;
    else if (request==BLKSSZGET) *(int *)arg=512;
    else if (request==BLKROGET) *(int *)arg=0;
    else if (request!=BLKRRPART) { errno=ENOTTY; return -1; }
    return 0;
}

/** @brief Write a minimal ext superblock feature fixture.
 * @param fd Open disposable image. @param compat Compatible features.
 * @param incompat Incompatible features. @param ro Read-only features.
 */
static void super_fixture(int fd,uint32_t compat,uint32_t incompat,uint32_t ro)
{
    uint8_t s[512]={0}; uint16_t magic=0xef53;
    memcpy(s+56,&magic,2); memcpy(s+92,&compat,4);
    memcpy(s+96,&incompat,4); memcpy(s+100,&ro,4);
    assert(pwrite(fd,s,sizeof(s),1024)==sizeof(s));
}

/** @brief Exercise ext feature classification, GPT type and real mke2fs formatting.
 * @param argc Number of arguments. @param argv Program and two disposable images.
 * @return Zero on success.
 */
int main(int argc,char **argv)
{
    assert(argc==3);
    int fd=open(argv[1],O_RDWR|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    assert(!ftruncate(fd,64LL*1024*1024));
    const struct { uint32_t compat,incompat,ro,want; } cases[]={
        {0,2,3,RELIEFOS_BLOCK_FILESYSTEM_EXT2},
        {4,2,3,RELIEFOS_BLOCK_FILESYSTEM_EXT3},
        {4,6,3,RELIEFOS_BLOCK_FILESYSTEM_EXT3},
        {4,0x42,3,RELIEFOS_BLOCK_FILESYSTEM_EXT4},
        {4,0x82,3,RELIEFOS_BLOCK_FILESYSTEM_EXT4},
        {4,2,0x403,RELIEFOS_BLOCK_FILESYSTEM_EXT4},
        {0,6,3,RELIEFOS_BLOCK_FILESYSTEM_EXT4},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        super_fixture(fd,cases[i].compat,cases[i].incompat,cases[i].ro);
        uint32_t found=0; assert(!reliefos_block_probe_filesystem(argv[1],&found));
        if (found!=cases[i].want) {
            fprintf(stderr,"feature case %u: expected %u, got %u\n",i,cases[i].want,found); return 1;
        }
    }
    assert(!strcmp(reliefos_block_filesystem_name(RELIEFOS_BLOCK_FILESYSTEM_EXT3),"ext3"));
    assert(!close(fd));
    assert(reliefos_block_format(argv[1],RELIEFOS_BLOCK_FILESYSTEM_EXT3,"12345678901234567")==-EINVAL);
    fail_exec=1;
    assert(reliefos_block_format(argv[1],RELIEFOS_BLOCK_FILESYSTEM_EXT3,"EXT3TEST")==-ENOENT);
    fail_exec=0;
    assert(!reliefos_block_format(argv[1],RELIEFOS_BLOCK_FILESYSTEM_EXT3,"EXT3TEST"));
    uint32_t found=0; assert(!reliefos_block_probe_filesystem(argv[1],&found));
    assert(found==RELIEFOS_BLOCK_FILESYSTEM_EXT3);
    fd=open(argv[1],O_RDONLY); assert(fd>=0);
    uint8_t s[512]; assert(pread(fd,s,sizeof(s),1024)==sizeof(s));
    uint32_t compat,incompat,ro; memcpy(&compat,s+92,4); memcpy(&incompat,s+96,4); memcpy(&ro,s+100,4);
    assert((compat&4) && !(compat&~0x3fu) && !(incompat&~6u) && !(ro&~7u));
    assert(!memcmp(s+120,"EXT3TEST",8)); assert(!close(fd));
    fd=open(argv[2],O_RDWR|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    assert(!ftruncate(fd,512LL*1024*1024)); assert(!close(fd));
    assert(!reliefos_block_gpt_initialize(argv[2],0));
    uint32_t index=99; assert(!reliefos_block_gpt_create(argv[2],RELIEFOS_BLOCK_FILESYSTEM_EXT3,64,"ext3",&index));
    assert(index==0);
    fd=open(argv[2],O_RDONLY); assert(fd>=0);
    const uint8_t linux_guid[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    uint8_t guid[16]; assert(pread(fd,guid,sizeof(guid),1024)==sizeof(guid));
    assert(!memcmp(guid,linux_guid,sizeof(guid))); assert(!close(fd));
    puts("PASS ext3 block probe, formatter and GPT type"); return 0;
}
