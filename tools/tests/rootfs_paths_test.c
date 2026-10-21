/* Link the real procfs and permission walker, with only disk/allocator mocks. */
#define main proc_fixture_main
#include "procfs_directories_test.c"
#undef main
#include <stdlib.h>
#include "../../kernel/reliefnt/fs/security/access.c"
int storage_node_mount_flags(const struct storage_node *node, uint64_t *flags)
{ (void)node; *flags = 0; return 0; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
int storage_lookup_path(const char *path, struct storage_node *node)
{
    if (!strcmp(path,"/") || !strcmp(path,"/etc") || !strcmp(path,"/home") || !strcmp(path,"/home/test")) {
        *node=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR}; return 0;
    }
    if (!strcmp(path,"/etc/mtab")) {
        *node=(struct storage_node){.type=RELIEFOS_FS_TYPE_SYMLINK}; return 0;
    }
    return -2;
}
int storage_readlink(const char *path,char *out,uint32_t capacity,uint32_t *length)
{
    assert(!strcmp(path,"/etc/mtab"));
    const char *target="../proc/mounts"; *length=strlen(target);
    assert(*length<capacity); memcpy(out,target,*length); return 0;
}
int storage_inode_permissions(const struct storage_node *node,struct reliefos_permissions *out,bool write)
{ (void)node; (void)out; (void)write; assert(0); return -95; }
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *out,bool write)
{ (void)node; (void)out; (void)write; assert(0); return -95; }
int storage_sidecar_permissions(const char *path, struct reliefos_permissions *value, bool write)
{
    (void)path;
    (void)write;
    *value = (struct reliefos_permissions){0755, 0, 0};
    return 0;
}
int main(void)
{
    current.uid=current.euid=current.fsuid=0;
    strcpy(sched_task_cwd(&current),"/home/test");
    char path[RELIEFOS_FS_PATH_LEN];
    assert(fs_permissions_resolve_flags(&current,"/","/etc/mtab",path,sizeof(path),false,FS_LOOKUP_FOLLOW)==0);
    assert(!strcmp(path,"/proc/42/mounts"));
    assert(fs_permissions_resolve_flags(&current,"/","/proc/self/mountinfo",path,sizeof(path),false,FS_LOOKUP_FOLLOW)==0);
    assert(!strcmp(path,"/proc/42/mountinfo"));
    assert(fs_permissions_resolve_flags(&current,"/","/proc/self/cwd",path,sizeof(path),false,FS_LOOKUP_FOLLOW)==0);
    assert(!strcmp(path,"/home/test"));
    assert(fs_permissions_resolve_flags(&current,"/","/proc/self",path,sizeof(path),false,0)==0);
    assert(!strcmp(path,"/proc/self"));
    puts("PASS actual permission walker follows mtab -> proc/mounts -> self/mounts, mountinfo and cwd");
}
