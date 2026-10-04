/* Production SHM registry, scheduler VMA hooks, memory syscalls and page tables.
 * Only CPU/physical allocator/user address boundaries are replaced by host code. */
#define RELIEFNT_TEST_REAL_SYSV_SHM
#define main audio_device_shm_fixture_main
#include "audio_device_test.c"
#undef main
static int shm_physical_budget = -1;
static bool shm_physical_gate(void)
{
    if (!shm_physical_budget) return false;
    if (shm_physical_budget > 0) --shm_physical_budget;
    return true;
}
#define RELIEFNT_TEST_PAGE_ALLOC_GATE shm_physical_gate
#define main paging_shm_fixture_main
#include "paging_protection_test.c"
#undef main
#undef RELIEFNT_TEST_PAGE_ALLOC_GATE
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"

static int shm_fail_heap = -1, shm_fail_page = -1;
static unsigned shm_heap_live;
static void *shm_test_malloc(size_t size)
{
    if (!shm_fail_heap) return NULL;
    if (shm_fail_heap > 0) --shm_fail_heap;
    void *p = kernel_malloc(size);
    if (p) ++shm_heap_live;
    return p;
}
static void shm_test_free(void *p)
{
    if (p) { assert(shm_heap_live); --shm_heap_live; }
    kernel_free(p);
}
static uint64_t shm_test_page(void)
{
    if (!shm_fail_page) return 0;
    if (shm_fail_page > 0) --shm_fail_page;
    uint64_t p = mm_alloc_page();
    memset((void *)(uintptr_t)p, 0xa5, 4096); /* Production must zero it. */
    return p;
}
#define kernel_malloc shm_test_malloc
#define kernel_free shm_test_free
#define mm_alloc_page shm_test_page
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_sysv_shm.c"
#undef kernel_malloc
#undef kernel_free
#undef mm_alloc_page

void storage_inode_retain(struct storage_inode_ref *inode) { assert(!inode); }
int storage_inode_put(struct storage_inode_ref *inode) { assert(!inode); return 0; }
/* Fork must fail before any descriptor/SEM_UNDO ownership is cloned in the
 * focused rollback case. Entering those unrelated backends is a fixture error. */
void arch_fpu_save(void *state) { (void)state; }
uint64_t time_uptime_ms(void) { return 1000; }
uint64_t mm_total_memory_kib(void) { return 1024 * 1024; }
int syscall_clone_task_files(const struct task *parent, struct task *child)
{ (void)parent; (void)child; abort(); }
void syscall_release_task_files(struct task *task) { (void)task; abort(); }
int task_sysv_sem_clone(struct task *parent, struct task *child, uint64_t flags)
{ (void)parent; (void)child; (void)flags; abort(); }
int user_copy_to_task(struct task *task, uint64_t address, const void *source, uint64_t size)
{
    assert(task == sched_current_task());
    if (!user_range_writable(address, size)) return -LINUX_EFAULT;
    memcpy((void *)(uintptr_t)address, source, size);
    return 0;
}
static unsigned live_pages(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 1024; ++i) n += !!pages[i].refs;
    return n;
}
static int64_t get(int key, uint64_t bytes, unsigned flags)
{ return syscall_sysv_shm(__NR_shmget, (uint32_t)key, bytes, flags); }
static int64_t ctl(int id, int command, void *out)
{ return syscall_sysv_shm(__NR_shmctl, (uint32_t)id, (uint32_t)command, (uintptr_t)out); }
static int64_t attach(int id, uint64_t address, unsigned flags)
{ return syscall_sysv_shm(__NR_shmat, (uint32_t)id, address, flags); }
static int64_t detach(uint64_t address)
{ return syscall_sysv_shm(__NR_shmdt, address, 0, 0); }

int main(void)
{
    struct task defaults = {0};
    task_init_limits(&defaults);
    assert(defaults.limits.memlock.rlim_cur == 8 * 1024 * 1024 &&
        defaults.limits.memlock.rlim_max == 8 * 1024 * 1024);
    struct task parent = {.pid = 801, .tgid = 801, .kind = TASK_KIND_USER,
        .euid = 1000, .egid = 100, .fsgid = 100};
    struct task child = {.pid = 802, .tgid = 802, .kind = TASK_KIND_USER,
        .euid = 1000, .egid = 100, .fsgid = 100};
    struct task *entries[] = {&parent, &child};
    tasks = entries; task_count = task_capacity = 2; current_pid[0] = parent.pid;
    parent.limits.as.rlim_cur = child.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    parent.limits.memlock.rlim_cur = child.limits.memlock.rlim_cur = 65536;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(&parent)));
    unsigned empty = live_pages();
    assert(get(IPC_PRIVATE, 0, 0600) == -LINUX_EINVAL);
    assert(get(0x1123, 0, 0) == -LINUX_ENOENT);
    assert(get(IPC_PRIVATE, UINT64_MAX, 0600) == -LINUX_EINVAL);
    shm_fail_heap = 0;
    assert(get(IPC_PRIVATE, 4096, 0600) == -LINUX_ENOMEM);
    shm_fail_heap = 1;
    assert(get(IPC_PRIVATE, 4096, 0600) == -LINUX_ENOMEM);
    shm_fail_heap = -1; shm_fail_page = 1;
    assert(get(IPC_PRIVATE, 8192, 0600) == -LINUX_ENOMEM);
    shm_fail_page = -1;
    assert(live_pages() == empty && !shm_heap_live && !sysv_shm_used);
    int id = (int)get(0x1123, 3 * 4096 - 1, IPC_CREAT | 0600);
    assert(id >= 0 && get(0x1123, 0, 0) == id);
    assert(get(0x1123, 20000, IPC_CREAT | IPC_EXCL) == -LINUX_EEXIST);
    assert(get(0x1123, 20000, 0) == -LINUX_EINVAL);
    struct shmid64_ds stat;
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_segsz == 12287 && !stat.shm_nattch);
    assert(ctl(id, IPC_STAT | 0x100, &stat) == -LINUX_EINVAL);
    assert(ctl(id, IPC_STAT, (void *)1) == -LINUX_EFAULT);
    assert(attach(id, 1, 0) == -LINUX_EINVAL);
    assert(attach(id, 0, SHM_REMAP) == -LINUX_EINVAL);
    shm_fail_heap = 0;
    assert(attach(id, 0, 0) == -LINUX_ENOMEM);
    shm_fail_heap = -1;
    long base = attach(id, 0, 0);
    assert(base > 0 && !ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    uint64_t backing = address_space_user_page_phys(sched_task_as(&parent), base);
    assert(backing && address_space_user_page_writable(sched_task_as(&parent), base));
    for (unsigned i = 0; i < 4096; ++i) assert(!((uint8_t *)(uintptr_t)backing)[i]);
    *(uint8_t *)(uintptr_t)backing = 0x4d;
    child.state = TASK_EXITED; child.running_cpu = SCHED_CPU_NONE;
    child.flags = TASK_FLAG_RESOURCES_RELEASED;
    shm_physical_budget = 0;
    assert(sched_clone_current(&parent.frame, 17, 0, 0, 0, 0) == -LINUX_ENOMEM);
    shm_physical_budget = -1;
    assert(child.running_cpu == SCHED_CPU_NONE);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    assert(parent.vmas[0].sysv_shm_attachment &&
        parent.vmas[0].sysv_shm_attachment->pieces == 1);
    shm_fail_heap = 0;
    assert(sched_clone_current(&parent.frame, 17, 0, 0, 0, 0) == -LINUX_ENOMEM);
    shm_fail_heap = -1;
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    child.pid = child.tgid = 802; child.kind = TASK_KIND_USER;
    child.state = TASK_READY;
    child.euid = 1000; child.egid = 100;
    assert(address_space_clone_cow(sched_task_as(&parent), sched_task_as(&child)));
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX; ++i) if (parent.vmas[i].used) {
        child.vmas[i] = parent.vmas[i]; child.vmas[i].sysv_shm_attachment = NULL;
        assert(!sysv_shm_vma_clone(&child, &child.vmas[i], &parent.vmas[i]));
    }
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 2);
    assert(address_space_user_page_phys(sched_task_as(&child), base) == backing);
    assert(!address_space_handle_cow_fault(sched_task_as(&child), base));
    *(uint8_t *)(uintptr_t)address_space_user_page_phys(sched_task_as(&child), base) = 0x2a;
    assert(*(uint8_t *)(uintptr_t)backing == 0x2a);
    current_pid[0] = child.pid;
    assert(!detach(base));
    sched_task_vma_release(&child); address_space_destroy(sched_task_as(&child));
    current_pid[0] = parent.pid;
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1 && stat.shm_dtime);
    long readonly = attach(id, 0, SHM_RDONLY);
    assert(readonly > 0 && !address_space_user_page_writable(sched_task_as(&parent), readonly));
    child.state = TASK_EXITED; child.running_cpu = SCHED_CPU_NONE;
    child.flags = TASK_FLAG_RESOURCES_RELEASED;
    shm_fail_heap = 1;
    assert(sched_clone_current(&parent.frame, 17, 0, 0, 0, 0) == -LINUX_ENOMEM);
    shm_fail_heap = -1;
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 2);
    child.pid = child.tgid = 802; child.kind = TASK_KIND_USER; child.state = TASK_READY;
    assert(syscall_mm_mprotect(readonly, 4096, 3) == -LINUX_EACCES);
    assert(!detach(readonly));
    assert(!syscall_mm_mprotect(base + 4096, 4096, LINUX_PROT_NONE));
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 3);
    assert(address_space_clone_cow(sched_task_as(&parent), sched_task_as(&child)));
    struct sysv_shm_attachment *child_identity = NULL;
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX; ++i) if (parent.vmas[i].used) {
        child.vmas[i] = parent.vmas[i]; child.vmas[i].sysv_shm_attachment = NULL;
        assert(!sysv_shm_vma_clone(&child, &child.vmas[i], &parent.vmas[i]));
        if (!child_identity) child_identity = child.vmas[i].sysv_shm_attachment;
        assert(child.vmas[i].sysv_shm_attachment == child_identity);
    }
    assert(child_identity && child_identity->pieces == 3);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 6);
    struct address_space exec_replacement = {0};
    assert(address_space_create(&exec_replacement));
    sched_exec_replace_mm(&child, &exec_replacement);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 3);
    address_space_destroy(sched_task_as(&child));
    assert(!syscall_mm_munmap(base + 4096, 4096));
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 2);
    assert(!ctl(id, IPC_RMID, NULL));
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_perm.key == IPC_PRIVATE);
    assert(get(0x1123, 0, 0) == -LINUX_ENOENT);
    long removed = attach(id, 0, 0);
    assert(removed > 0);
    assert(*(uint8_t *)(uintptr_t)address_space_user_page_phys(sched_task_as(&parent), removed) == 0x2a);
    assert(!detach(removed) && !detach(base));
    assert(ctl(id, IPC_STAT, &stat) == -LINUX_EINVAL && !sysv_shm_used && !shm_heap_live);
    assert(detach(base) == -LINUX_EINVAL);
    assert(!ctl(0, IPC_INFO, &(struct shminfo64){0}));
    struct shm_info info;
    assert(!ctl(0, SHM_INFO, &info) && !info.used_ids && !info.shm_tot);
    id = (int)get(IPC_PRIVATE, 8192, 0600);
    base = attach(id, 0, 0);
    assert(base > 0 && !ctl(id, IPC_RMID, NULL));
    assert(attach(id, base, SHM_REMAP) == base);
    assert(!detach(base) && !sysv_shm_used && !shm_heap_live);
    id = (int)get(IPC_PRIVATE, 12288, 0600);
    base = attach(id, 0, 0);
    int replacement = (int)get(IPC_PRIVATE, 4096, 0600);
    assert(attach(replacement, base, SHM_REMAP) == base);
    assert(!detach(base));
    assert(!ctl(replacement, IPC_STAT, &stat) && !stat.shm_nattch);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    assert(!detach(base) && !ctl(id, IPC_RMID, NULL) && !ctl(replacement, IPC_RMID, NULL));
    /* Force metadata growth while mprotect retains a VMA in the growable
     * array. ASan must catch stale pointers across scheduler reallocations. */
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX; ++i)
        parent.vmas[i] = (struct task_vma){.used = 1,
            .start = RELIEFNT_USER_BASE + i * 4096,
            .end = RELIEFNT_USER_BASE + (i + 1) * 4096};
    id = (int)get(IPC_PRIVATE, 12288, 0600);
    base = attach(id, RELIEFNT_USER_BASE + 0x400000, 0);
    assert(base > 0);
    assert(!syscall_mm_mprotect(base + 4096, 4096, LINUX_PROT_NONE));
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 3);
    assert(!detach(base) && !ctl(id, IPC_RMID, NULL));
    uint64_t crossing = RELIEFNT_USER_BASE + 24 * RELIEFNT_USER_PD_BYTES - 4096;
    id = (int)get(IPC_PRIVATE, 4096, 0600);
    base = attach(id, crossing, 0);
    assert(base == (long)crossing);
    backing = address_space_user_page_phys(sched_task_as(&parent), base);
    *(uint8_t *)(uintptr_t)backing = 0x77;
    replacement = (int)get(IPC_PRIVATE, 8192, 0600);
    unsigned before_tables = live_pages();
    shm_physical_budget = 0;
    assert(attach(replacement, base, SHM_REMAP) == -LINUX_ENOMEM);
    shm_physical_budget = -1;
    assert(live_pages() == before_tables &&
        address_space_user_page_phys(sched_task_as(&parent), base) == backing &&
        *(uint8_t *)(uintptr_t)backing == 0x77);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    assert(!ctl(replacement, IPC_STAT, &stat) && !stat.shm_nattch);
    assert(!detach(base) && !ctl(id, IPC_RMID, NULL) && !ctl(replacement, IPC_RMID, NULL));
    memset(parent.vmas, 0, sizeof(parent.vmas));
    id = (int)get(IPC_PRIVATE, 4096, 0600);
    assert(!ctl(id, SHM_LOCK, NULL));
    parent.limits.memlock.rlim_cur = 0;
    assert(ctl(id, SHM_LOCK, NULL) == -LINUX_EPERM);
    assert(!ctl(id, SHM_UNLOCK, NULL) && !ctl(id, IPC_RMID, NULL));
    parent.limits.memlock.rlim_cur = 65536;
    id = (int)get(0x5566, 8192, IPC_CREAT | 0640 | SHM_NORESERVE);
    assert(id >= 0 && !ctl(id, IPC_STAT, &stat));
    int slot = id & (SYSV_SHM_SLOTS - 1);
    assert(ctl(slot, SHM_STAT, &stat) == id && ctl(slot, SHM_STAT_ANY, &stat) == id);
    parent.euid = 2000; parent.egid = 200;
    assert(get(0x5566, 0, 0400) == -LINUX_EACCES);
    assert(attach(id, 0, SHM_RDONLY) == -LINUX_EACCES);
    assert(ctl(id, IPC_STAT, &stat) == -LINUX_EACCES);
    assert(ctl(id, IPC_RMID, NULL) == -LINUX_EPERM);
    assert(ctl(id, IPC_SET, &stat) == -LINUX_EPERM);
    assert(ctl(slot, SHM_STAT_ANY, &stat) == id);
    struct task_groups *groups = malloc(sizeof(*groups) + sizeof(groups->ids[0]));
    assert(groups); groups->count = 1; groups->ids[0] = 100; parent.groups = groups;
    base = attach(id, 0, SHM_RDONLY);
    assert(base > 0 && !detach(base));
    assert(attach(id, 0, 0) == -LINUX_EACCES);
    parent.groups = NULL; free(groups);
    parent.cap_effective = 1ULL << CAP_IPC_OWNER;
    base = attach(id, 0, 0);
    assert(base > 0 && !detach(base) && !ctl(id, IPC_STAT, &stat));
    assert(ctl(id, IPC_RMID, NULL) == -LINUX_EPERM);
    parent.cap_effective = 1ULL << CAP_SYS_ADMIN;
    stat.shm_perm.mode = 0700;
    assert(!ctl(id, IPC_SET, &stat));
    parent.cap_effective = 0; parent.euid = 1000; parent.egid = 100;
    base = attach(id, 0, SHM_RDONLY | SHM_EXEC);
    assert(base > 0 && !address_space_user_page_writable(sched_task_as(&parent), base));
    assert(!detach(base));
    base = attach(id, RELIEFNT_USER_BASE + 0x800001, SHM_RND);
    assert(base == RELIEFNT_USER_BASE + 0x800000);
    assert(attach(id, base, 0) == -LINUX_EINVAL);
    assert(!detach(base));
    assert(attach(id, 0, SHM_EXEC) == -LINUX_EACCES); /* Platform W^X. */
    assert(ctl(id, IPC_SET, (void *)1) == -LINUX_EFAULT);
    stat.shm_perm.uid = UINT32_MAX;
    assert(ctl(id, IPC_SET, &stat) == -LINUX_EINVAL);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_perm.uid == 1000);
    parent.limits.memlock.rlim_cur = 4096;
    assert(ctl(id, SHM_LOCK, NULL) == -LINUX_ENOMEM);
    parent.euid = 2000;
    assert(ctl(id, SHM_LOCK, NULL) == -LINUX_EPERM);
    parent.cap_effective = 1ULL << CAP_IPC_LOCK;
    assert(!ctl(id, SHM_LOCK, NULL) && !ctl(id, SHM_UNLOCK, NULL));
    parent.euid = 1000; parent.cap_effective = 0;
    parent.limits.memlock.rlim_cur = 8192;
    assert(!ctl(id, SHM_LOCK, NULL));
    replacement = (int)get(IPC_PRIVATE, 4096, 0600);
    assert(ctl(replacement, SHM_LOCK, NULL) == -LINUX_ENOMEM);
    assert(!ctl(id, IPC_STAT, &stat) && (stat.shm_perm.mode & 02000));
    stat.shm_perm.mode = 0600;
    assert(!ctl(id, IPC_SET, &stat) && !ctl(id, IPC_STAT, &stat));
    assert(stat.shm_perm.mode & 02000);
    assert(!ctl(id, SHM_UNLOCK, NULL) && !ctl(replacement, SHM_LOCK, NULL));
    assert(!ctl(replacement, IPC_RMID, NULL) && !ctl(id, IPC_RMID, NULL));
    assert(get(IPC_PRIVATE, 4096, 0600 | SHM_HUGETLB) == -LINUX_ENOSYS);
    id = (int)get(IPC_PRIVATE, 4096, 0600);
    assert(id >= 0 && !ctl(id, IPC_RMID, NULL));
    bool reused = false;
    for (unsigned i = 0; i < SYSV_SHM_SLOTS; ++i) {
        replacement = (int)get(IPC_PRIVATE, 4096, 0600);
        if ((replacement & (SYSV_SHM_SLOTS - 1)) == (id & (SYSV_SHM_SLOTS - 1))) {
            assert(replacement != id);
            assert(ctl(id, IPC_STAT, &stat) == -LINUX_EINVAL);
            assert(attach(id, 0, 0) == -LINUX_EINVAL);
            reused = true;
        }
        assert(!ctl(replacement, IPC_RMID, NULL));
    }
    assert(reused && !sysv_shm_used && !sysv_shm_pages && !shm_heap_live);
    id = (int)get(IPC_PRIVATE, 8192, 0600);
    crossing = RELIEFNT_USER_BASE + 16 * RELIEFNT_USER_PD_BYTES - 4096;
    before_tables = live_pages();
    shm_physical_budget = 1;
    assert(attach(id, crossing, 0) == -LINUX_ENOMEM);
    shm_physical_budget = -1;
    assert(live_pages() == before_tables);
    assert(!ctl(id, IPC_STAT, &stat) && !stat.shm_nattch);
    base = attach(id, crossing, 0);
    assert(base == (long)crossing);
    assert(address_space_clone_cow(sched_task_as(&parent), sched_task_as(&child)));
    struct task_vma *source = task_vma_containing(&parent, base, base + 8192);
    child.vmas[0] = *source;
    child.vmas[0].sysv_shm_attachment = NULL;
    shm_fail_heap = 0;
    assert(sysv_shm_vma_clone(&child, &child.vmas[0], source) == -LINUX_ENOMEM);
    shm_fail_heap = -1;
    assert(!child.vmas[0].sysv_shm_attachment);
    sched_task_vma_release(&child); address_space_destroy(sched_task_as(&child));
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    assert(!detach(base) && !ctl(id, IPC_RMID, NULL));
    id = (int)get(IPC_PRIVATE, 4096, 0600);
    base = attach(id, 0, 0);
    assert(base > 0 && !ctl(id, IPC_RMID, NULL));
    assert(!task_promote_shared(&parent, CLONE_VM));
    child.shared_mm = parent.shared_mm;
    ++child.shared_mm->references;
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    exec_replacement = (struct address_space){0};
    assert(address_space_create(&exec_replacement));
    sched_exec_replace_mm(&child, &exec_replacement);
    assert(!ctl(id, IPC_STAT, &stat) && stat.shm_nattch == 1);
    assert(address_space_user_page_phys(sched_task_as(&parent), base));
    address_space_destroy(sched_task_as(&child));
    exec_replacement = (struct address_space){0};
    assert(address_space_create(&exec_replacement));
    sched_exec_replace_mm(&parent, &exec_replacement);
    assert(ctl(id, IPC_STAT, &stat) == -LINUX_EINVAL && !shm_heap_live);
    address_space_destroy(sched_task_as(&parent));
    assert(!live_pages());
    tasks = NULL; task_count = 0; current_pid[0] = 0;
    puts("PASS real SysV SHM registry/PTE/VMA: zero pages, independent MMs, fork sharing,");
    puts("readonly max_prot, split nattch, RMID live IDs, shmdt pieces, failure rollback and final page reclamation");
}
