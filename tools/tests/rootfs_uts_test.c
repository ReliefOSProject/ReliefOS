#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/init/uts.c"
static const char *config="test-host\n";
static unsigned locked;
static int read_error, lookup_error;
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; assert(!locked); locked=1; *flags=0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; assert(locked); locked=0; }
int storage_lookup_path(const char *path, struct storage_node *node)
{ assert(!strcmp(path,"/etc/hostname")); *node=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,.size=strlen(config)}; return lookup_error; }
int storage_read_node(const struct storage_node *node,uint64_t offset,void *out,uint32_t cap,uint32_t *got)
{ assert(!locked && !offset && node->size<=cap); memcpy(out,config,node->size); *got=node->size; return read_error; }
int main(void)
{
    char host[65],domain[65],full[64]; memset(full,'x',sizeof(full));
    assert(linux_uts_load_hostname()==0); linux_uts_names(host,domain);
    assert(!strcmp(host,"test-host") && !strcmp(domain,"(none)"));
    assert(linux_uts_set(full,64,0)==0); linux_uts_names(host,domain); assert(!memcmp(host,full,64) && !host[64]);
    assert(linux_uts_set(full,65,0)==-22);
    assert(linux_uts_set("abc",3,1)==0); linux_uts_names(host,domain); assert(!strcmp(domain,"abc"));
    assert(linux_uts_set(NULL,0,0)==0); linux_uts_names(host,domain); assert(!host[0]);
    config="host\nsecond"; assert(linux_uts_load_hostname()==-22);
    config="host\n"; read_error=-5; assert(linux_uts_load_hostname()==-5);
    lookup_error=-2; assert(linux_uts_load_hostname()==0);
    puts("PASS hostname initialization, shared UTS names, truncation, invalid configuration and I/O failure");
}
