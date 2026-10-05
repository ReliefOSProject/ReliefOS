/* Non-SHM fixtures link scheduler/MM code but never create SysV segments.
 * Unexpected SHM metadata is an assertion failure, not simulated SHM proof.
 * The dedicated SHM fixture links the actual registry and lifecycle functions. */
#ifndef RELIEFOS_SYSV_SHM_FIXTURE_STUBS_H
#define RELIEFOS_SYSV_SHM_FIXTURE_STUBS_H
#ifndef RELIEFNT_TEST_REAL_SYSV_SHM
#include <assert.h>
#include <reliefnt/sysv_shm.h>
void sysv_shm_vma_split(struct task_vma *vma)
{ assert(!vma || !vma->sysv_shm_attachment); }
void sysv_shm_vma_release(struct task *task, struct task_vma *vma)
{ (void)task; assert(!vma || !vma->sysv_shm_attachment); }
int sysv_shm_vma_clone(struct task *task, struct task_vma *dst, const struct task_vma *src)
{ (void)task; assert(!src->sysv_shm_attachment); dst->sysv_shm_attachment = NULL; return 0; }
#endif
#endif
