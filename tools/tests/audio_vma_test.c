/* Real PCM, scheduler VMA hooks, memory syscalls and page tables. Only CPU,
 * allocation and the hardware card are host boundaries. */
#define main audio_device_fixture_main
#include "audio_device_test.c"
#undef main
#define main paging_fixture_main
#include "paging_protection_test.c"
#undef main
#include "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"

void storage_inode_retain(struct storage_inode_ref *inode) { assert(!inode); }
int storage_inode_put(struct storage_inode_ref *inode) { assert(!inode); return 0; }

int main(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    struct audio_params params = {48000, 2, 16, 4, 512, 4096};
    assert(!audio_pcm_hw_params(pcm, &params));
    assert(!audio_pcm_prepare(pcm));
    struct audio_pcm_mmap mapping;
    assert(!audio_pcm_mmap_acquire(pcm, 0, 16384, 3, &mapping));
    struct task parent_storage, child_storage;
    struct task *parent = &parent_storage, *child = &child_storage;
    struct task *entries[] = {parent, child};
    tasks = entries; task_count = 2;
    *parent = (struct task){.pid = 1, .kind = TASK_KIND_USER};
    *child = (struct task){.pid = 2, .kind = TASK_KIND_USER};
    current_pid[0] = 1;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(parent)));
    const uint64_t va = RELIEFNT_USER_BASE;
    for (unsigned i = 0; i < 4; ++i)
        assert(address_space_map_user_page(sched_task_as(parent), va + i * 4096,
            mapping.backing + i * 4096,
            RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_WRITABLE));
    parent->vmas[0] = (struct task_vma){.used = 1, .start = va, .end = va + 16384,
        .prot = 3, .max_prot = 3, .flags = TASK_VMA_FLAG_DEVICE | TASK_VMA_FLAG_SHARED,
        .audio_pcm = pcm, .audio_mmap_region = mapping.region,
        .audio_mmap_offset = mapping.offset, .audio_mmap_length = mapping.length,
        .audio_mmap_prot = mapping.prot, .audio_mmap_generation = mapping.generation};
    assert(!syscall_mm_mprotect(va + 4096, 4096, LINUX_PROT_NONE));
    assert(!address_space_user_page_readable(sched_task_as(parent), va + 4096));
    assert(pcm->mapping_refs == 3);
    assert(!syscall_mm_munmap(va + 8192, 4096));
    assert(pcm->mapping_refs == 3);
    assert(address_space_clone_cow(sched_task_as(parent), sched_task_as(child)));
    for (unsigned i = 0; i < SCHED_TASK_VMA_MAX; ++i) if (parent->vmas[i].used) {
        child->vmas[i] = parent->vmas[i];
        assert(!sched_task_vma_retain_audio(&child->vmas[i], &parent->vmas[i]));
    }
    assert(pcm->mapping_refs == 6);
    audio_pcm_release(pcm);
    assert(pcm->live && !pcm->file_refs);
    assert(!syscall_mm_mprotect(va + 4096, 4096, LINUX_PROT_READ));
    assert(!syscall_mm_munmap(va, 16384));
    assert(pcm->mapping_refs == 3);
    address_space_destroy(sched_task_as(parent));
    assert(address_space_user_page_phys(sched_task_as(child), va) == mapping.backing);
    assert(!audio_unregister_card(card.id, 0));
    current_pid[0] = 2;
    assert(!syscall_mm_munmap(va, 16384));
    assert(!pcm->live && !pcm->mapping_refs);
    sched_task_vma_release(child);
    address_space_destroy(sched_task_as(child));
    for (unsigned i = 0; i < 1024; ++i) assert(!pages[i].refs);
    puts("PASS audio VMA: partial mprotect/munmap, PROT_NONE fork, fd close, disconnect, last unmap");
}
