#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <grp.h>
#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Identical native syscall probe for Linux reference and the target guest.
 * Every segment is IPC_PRIVATE and is removed by this process on every exit. */
static int ids[16], id_count, checks;
static unsigned char *clone_memory;
static int clone_id;
static void cleanup(void)
{
    for (int i = 0; i < id_count; ++i)
        if (ids[i] >= 0) syscall(SYS_shmctl, ids[i], IPC_RMID, 0);
}
static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL native SHM %s errno=%d\n", name, errno);
        exit(1);
    }
    ++checks;
}
static long control(int id, int cmd, void *out)
{ return syscall(SYS_shmctl, id, cmd, out); }
static int create(size_t size)
{
    long id = syscall(SYS_shmget, IPC_PRIVATE, size, 0600);
    check(id >= 0 && id_count < 16, "create-owned-private");
    ids[id_count++] = (int)id;
    return (int)id;
}
static int clone_shared_mm(void *unused)
{
    (void)unused;
    struct shmid_ds st;
    if (control(clone_id, IPC_STAT, &st) || st.shm_nattch != 1) return 2;
    clone_memory[0] = 0x61;
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--exec-check")) {
        struct shmid_ds st;
        return control((int)strtol(argv[2], NULL, 10), IPC_STAT, &st) ||
            st.shm_nattch != 1;
    }
    check(!atexit(cleanup), "cleanup-registered");
    /* Permission checks need an ordinary IPC caller. Guest QA commonly starts
     * as root: drop only this probe's IDs/capabilities before creating objects. */
    if (!geteuid())
        check(!setgroups(0, NULL) && !setgid(65534) && !setuid(65534), "drop-probe-root-credentials");
    check(sizeof(struct shmid_ds) == 112 &&
          offsetof(struct shmid_ds, shm_segsz) == 48 &&
          offsetof(struct shmid_ds, shm_nattch) == 88, "native-LP64-layout");
    errno = 0;
    check(syscall(SYS_shmget, IPC_PRIVATE, 0, 0600) == -1 && errno == EINVAL,
          "zero-size-private");
    int id = create(3 * 4096 - 1);
    struct shmid_ds st;
    check(!control(id, IPC_STAT, &st) && st.shm_segsz == 3 * 4096 - 1 &&
          st.shm_nattch == 0 && st.shm_cpid == getpid(), "stat-created");
    errno = 0;
    check(control(id, IPC_STAT | 0x100, &st) == -1 && errno == EINVAL,
          "native-rejects-IPC64-command-bit");
    errno = 0;
    check(control(id, IPC_STAT, (void *)1) == -1 && errno == EFAULT, "bad-output");
    errno = 0;
    check(syscall(SYS_shmat, id, 0, SHM_REMAP) == -1 && errno == EINVAL,
          "remap-needs-address");
    void *memory = (void *)syscall(SYS_shmat, id, 0, 0);
    check(memory != (void *)-1, "attach");
    int all_zero = 1;
    for (size_t i = 0; i < 3 * 4096; ++i)
        all_zero &= !((unsigned char *)memory)[i];
    check(all_zero, "all-allocated-page-bytes-zero");
    memset(memory, 0x4d, 3 * 4096);
    check(!control(id, IPC_STAT, &st) && st.shm_nattch == 1 && st.shm_atime,
          "stat-attached");
    pid_t child = fork();
    check(child >= 0, "fork");
    if (!child) {
        if (((unsigned char *)memory)[4096] != 0x4d ||
            control(id, IPC_STAT, &st) || st.shm_nattch != 2) _exit(2);
        ((unsigned char *)memory)[4096] = 0x2a;
        _exit(0);
    }
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          !WEXITSTATUS(status), "fork-shared-attachment");
    check(((unsigned char *)memory)[4096] == 0x2a, "shared-write");
    check(!control(id, IPC_STAT, &st) && st.shm_nattch == 1 && st.shm_dtime,
          "exit-detaches");
    void *clone_stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(clone_stack != MAP_FAILED, "clone-stack");
    clone_memory = memory; clone_id = id;
    child = clone(clone_shared_mm, (char *)clone_stack + 65536, CLONE_VM | SIGCHLD, NULL);
    check(child >= 0 && waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && !WEXITSTATUS(status), "CLONE_VM-keeps-one-attachment");
    check(((unsigned char *)memory)[0] == 0x61 && !munmap(clone_stack, 65536),
          "CLONE_VM-shared-data");
    ((unsigned char *)memory)[0] = 0x4d;
    child = fork();
    check(child >= 0, "exec-fork");
    if (!child) {
        char number[32];
        snprintf(number, sizeof(number), "%d", id);
        char *args[] = {argv[0], "--exec-check", number, NULL};
        execv(argv[0], args);
        _exit(3);
    }
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          !WEXITSTATUS(status), "exec-detaches-inherited-attachment");
    void *readonly = (void *)syscall(SYS_shmat, id, 0, SHM_RDONLY);
    check(readonly != (void *)-1, "readonly-attach");
    errno = 0;
    check(mprotect(readonly, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES,
          "readonly-max-prot");
    check(!syscall(SYS_shmdt, readonly), "readonly-detach");
    check(!mprotect((char *)memory + 4096, 4096, PROT_NONE), "split-protect");
    check(!control(id, IPC_STAT, &st) && st.shm_nattch == 3, "split-counts-VMA-pieces");
    check(!munmap((char *)memory + 4096, 4096), "partial-unmap");
    /* Linux currently reports VM pieces in nattch after a split; log that
     * fact rather than assuming one count per original attachment. */
    check(!control(id, IPC_STAT, &st) && st.shm_nattch == 2, "stat-split");
    printf("native SHM split nattch=%lu\n", (unsigned long)st.shm_nattch);
    check(!control(id, IPC_RMID, NULL), "rmid-attached");
    check(!control(id, IPC_STAT, &st), "rmid-retains-ID");
    key_t reported_key;
    memcpy(&reported_key, &st.shm_perm, sizeof(reported_key));
    check(reported_key == IPC_PRIVATE, "rmid-hides-key");
    void *removed = (void *)syscall(SYS_shmat, id, 0, 0);
    check(removed != (void *)-1 && ((unsigned char *)removed)[0] == 0x4d,
          "attach-removed-live-ID");
    check(!syscall(SYS_shmdt, removed), "detach-removed-new-map");
    check(!syscall(SYS_shmdt, memory), "detach-all-original-split-pieces");
    errno = 0;
    check(control(id, IPC_STAT, &st) == -1 && (errno == EINVAL || errno == EIDRM),
          "last-detach-destroys");
    ids[0] = -1;
    errno = 0;
    check(syscall(SYS_shmdt, memory) == -1 && errno == EINVAL, "repeat-detach");
    int next = create(4096);
    check(next != id, "stale-ID-not-reused");
    check(!control(next, IPC_STAT, &st), "stat-second");
    st.shm_perm.mode = 0640;
    check(!control(next, IPC_SET, &st), "set-mode");
    check(!control(next, IPC_STAT, &st) && (st.shm_perm.mode & 0777) == 0640,
          "set-mode-roundtrip");
    check(!control(next, IPC_RMID, NULL), "remove-unattached");
    ids[1] = -1;
    int original = create(3 * 4096), replacement = create(4096);
    void *base = (void *)syscall(SYS_shmat, original, 0, 0);
    check(base != (void *)-1, "remap-original");
    check((void *)syscall(SYS_shmat, replacement, base, SHM_REMAP) == base,
          "remap-prefix");
    check(!syscall(SYS_shmdt, base), "detach-replacement-before-old-tail");
    check(!control(replacement, IPC_STAT, &st) && !st.shm_nattch,
          "replacement-detached");
    check(!control(original, IPC_STAT, &st) && st.shm_nattch == 1,
          "old-tail-retained");
    check(!syscall(SYS_shmdt, base), "detach-old-tail-by-original-base");
    check(!control(original, IPC_RMID, NULL) && !control(replacement, IPC_RMID, NULL),
          "remap-owned-cleanup");
    ids[2] = ids[3] = -1;
    original = create(4096);
    base = (void *)syscall(SYS_shmat, original, 0, 0);
    check(base != (void *)-1 && !control(original, IPC_RMID, NULL), "remap-removed-sole-map");
    check((void *)syscall(SYS_shmat, original, base, SHM_REMAP) == base,
          "remap-retains-removed-backing");
    check(!syscall(SYS_shmdt, base), "remap-removed-final-detach");
    ids[4] = -1;
    original = create(4096);
    struct shminfo limits;
    struct shm_info usage;
    check(sizeof(limits) == 72 && sizeof(usage) == 48, "native-info-layouts");
    long highest = control(0, IPC_INFO, &limits);
    check(highest >= 0 && limits.shmmin == 1 && limits.shmmax >= 4096 &&
          limits.shmmni && limits.shmall, "IPC_INFO-real-limits");
    check(control(0, SHM_INFO, &usage) >= 0 && usage.used_ids >= 1 &&
          usage.shm_tot >= 1, "SHM_INFO-real-usage");
    int slot = -1;
    for (long i = 0; i <= highest; ++i)
        if (control((int)i, SHM_STAT_ANY, &st) == original) { slot = (int)i; break; }
    check(slot >= 0 && control(slot, SHM_STAT, &st) == original, "SHM_STAT-slot-ID");
    errno = 0;
    check(control(original, IPC_SET, (void *)1) == -1 && errno == EFAULT, "bad-IPC_SET-input");
    errno = 0;
    check(control(original, 0x7fffffff, &st) == -1 && errno == EINVAL, "unknown-command");
    struct rlimit saved;
    check(!getrlimit(RLIMIT_MEMLOCK, &saved), "get-memlock");
    check(!control(original, SHM_LOCK, NULL), "lock-owned-resident-segment");
    struct rlimit zero = saved; zero.rlim_cur = 0;
    check(!setrlimit(RLIMIT_MEMLOCK, &zero), "lower-process-memlock");
    errno = 0;
    check(control(original, SHM_LOCK, NULL) == -1 && errno == EPERM,
          "repeated-lock-rechecks-zero-limit");
    check(!setrlimit(RLIMIT_MEMLOCK, &saved), "restore-process-memlock");
    check(!control(original, SHM_UNLOCK, NULL), "unlock-owned-segment");
    check(!control(original, IPC_STAT, &st), "read-permissions-before-denial");
    st.shm_perm.mode = 0;
    check(!control(original, IPC_SET, &st), "set-no-DAC-access");
    errno = 0;
    check(control(original, IPC_STAT, &st) == -1 && errno == EACCES, "owner-DAC-stat-denied");
    check(control(slot, SHM_STAT_ANY, &st) == original, "SHM_STAT_ANY-bypasses-DAC");
    st.shm_perm.mode = 0700;
    check(!control(original, IPC_SET, &st), "owner-can-restore-mode");
    base = (void *)syscall(SYS_shmat, original, 0, SHM_RDONLY | SHM_EXEC);
    check(base != (void *)-1 && !syscall(SYS_shmdt, base), "readonly-executable-attach");
    base = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(base != MAP_FAILED && !munmap(base, 8192), "reserve-rounding-address");
    check((void *)syscall(SYS_shmat, original, (char *)base + 1, SHM_RND) == base,
          "SHM_RND-page-alignment");
    check(!syscall(SYS_shmdt, base) && !control(original, IPC_RMID, NULL), "flag-owned-cleanup");
    ids[5] = -1;
    key_t key = (key_t)(0x51000000u | ((unsigned)getpid() & 0x00ffffffu));
    long keyed = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt, ++key) {
        errno = 0;
        keyed = syscall(SYS_shmget, key, 4096, IPC_CREAT | IPC_EXCL | 0600);
        if (keyed >= 0 || errno != EEXIST) break;
    }
    check(keyed >= 0 && id_count < 16, "create-owned-unique-key");
    ids[id_count++] = (int)keyed;
    check(syscall(SYS_shmget, key, 0, 0) == keyed, "key-zero-size-lookup");
    errno = 0;
    check(syscall(SYS_shmget, key, 8192, IPC_CREAT | IPC_EXCL) == -1 && errno == EEXIST,
          "key-exclusive-precedes-size-check");
    errno = 0;
    check(syscall(SYS_shmget, key, 8192, 0) == -1 && errno == EINVAL,
          "key-too-large-lookup");
    check(!control((int)keyed, IPC_RMID, NULL), "remove-owned-key");
    ids[id_count - 1] = -1;
    errno = 0;
    check(syscall(SYS_shmget, key, 0, 0) == -1 && errno == ENOENT, "removed-key-invisible");
    printf("PASS native SysV SHM checks=%d; all owned segments removed\n", checks);
    return 0;
}
