#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <linux/shm.h>

int main(void)
{
    _Static_assert(sizeof(struct shmid64_ds) == 112, "canonical x86_64 shmid64_ds");
    _Static_assert(offsetof(struct shmid64_ds, shm_segsz) == 48, "shm_segsz");
    _Static_assert(offsetof(struct shmid64_ds, shm_nattch) == 88, "shm_nattch");
    _Static_assert(sizeof(struct shminfo64) == 72, "canonical x86_64 shminfo64");
    _Static_assert(sizeof(struct shm_info) == 48, "canonical x86_64 shm_info");
    _Static_assert(sizeof(struct ipc64_perm) == 48, "canonical x86_64 ipc64_perm");
    assert(SHM_RDONLY == 010000 && SHM_STAT_ANY == 15);
    assert(SHM_HUGE_2MB == (21U << SHM_HUGE_SHIFT));
    printf("wire.ipc64_perm=%zu wire.shmid64_ds=%zu wire.shminfo64=%zu wire.shm_info=%zu\n",
        sizeof(struct ipc64_perm), sizeof(struct shmid64_ds), sizeof(struct shminfo64), sizeof(struct shm_info));
#define FIELD(type, name) printf(#type "." #name "=%zu\n", offsetof(struct type, name))
    FIELD(shmid64_ds, shm_perm); FIELD(shmid64_ds, shm_segsz);
    FIELD(shmid64_ds, shm_atime); FIELD(shmid64_ds, shm_dtime); FIELD(shmid64_ds, shm_ctime);
    FIELD(shmid64_ds, shm_cpid); FIELD(shmid64_ds, shm_lpid); FIELD(shmid64_ds, shm_nattch);
    FIELD(ipc64_perm, key); FIELD(ipc64_perm, uid); FIELD(ipc64_perm, gid);
    FIELD(ipc64_perm, cuid); FIELD(ipc64_perm, cgid); FIELD(ipc64_perm, mode); FIELD(ipc64_perm, seq);
    FIELD(shm_info, used_ids); FIELD(shm_info, shm_tot); FIELD(shm_info, shm_rss);
    FIELD(shm_info, shm_swp); FIELD(shm_info, swap_attempts); FIELD(shm_info, swap_successes);
#undef FIELD
#define VALUE(name) printf(#name "=%lu\n", (unsigned long)(name))
    VALUE(IPC_PRIVATE); VALUE(IPC_CREAT); VALUE(IPC_EXCL); VALUE(IPC_NOWAIT);
    VALUE(IPC_RMID); VALUE(IPC_SET); VALUE(IPC_STAT); VALUE(IPC_INFO);
    VALUE(SHMMIN); VALUE(SHMMNI); VALUE(SHMMAX); VALUE(SHMALL); VALUE(SHMSEG);
    VALUE(SHM_R); VALUE(SHM_W); VALUE(SHM_HUGETLB); VALUE(SHM_NORESERVE);
    VALUE(SHM_HUGE_SHIFT); VALUE(SHM_HUGE_MASK); VALUE(SHM_HUGE_64KB);
    VALUE(SHM_HUGE_512KB); VALUE(SHM_HUGE_1MB); VALUE(SHM_HUGE_2MB);
    VALUE(SHM_HUGE_8MB); VALUE(SHM_HUGE_16MB); VALUE(SHM_HUGE_32MB);
    VALUE(SHM_HUGE_256MB); VALUE(SHM_HUGE_512MB); VALUE(SHM_HUGE_1GB);
    VALUE(SHM_HUGE_2GB); VALUE(SHM_HUGE_16GB);
    VALUE(SHM_RDONLY); VALUE(SHM_RND); VALUE(SHM_REMAP); VALUE(SHM_EXEC);
    VALUE(SHM_LOCK); VALUE(SHM_UNLOCK); VALUE(SHM_STAT); VALUE(SHM_INFO); VALUE(SHM_STAT_ANY);
#undef VALUE
    return 0;
}
