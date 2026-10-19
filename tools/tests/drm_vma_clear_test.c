#include <assert.h>
#include <stdio.h>
#include <reliefnt/drm.h>
#include <reliefnt/sched.h>

static struct drm_bo *expected_bo;
static unsigned drm_bo_put_calls;

struct task *sched_current_task(void)
{
    return NULL;
}

void drm_bo_put(struct drm_bo *bo)
{
    assert(bo == expected_bo);
    ++drm_bo_put_calls;
}

void sysv_shm_vma_release(struct task *task, struct task_vma *vma)
{
    (void)task;
    (void)vma;
}

int storage_inode_put(struct storage_inode_ref *reference)
{
    assert(!reference);
    return 0;
}

void task_file_put(struct task_file *description)
{
    assert(!description);
}

#ifndef DRM_MM_SOURCE
#define DRM_MM_SOURCE "../../kernel/reliefnt/kernel/reliefnt/syscall_mm.c"
#endif
#include DRM_MM_SOURCE

int main(void)
{
    expected_bo = (struct drm_bo *)(uintptr_t)0x1000;
    struct task_vma vma = {
        .used = 1,
        .drm_bo = expected_bo,
    };
    task_vma_clear(&vma);
    assert(drm_bo_put_calls == 1);
    assert(!vma.used && !vma.drm_bo);
    puts("drm-vma-clear: PASS");
    return 0;
}
