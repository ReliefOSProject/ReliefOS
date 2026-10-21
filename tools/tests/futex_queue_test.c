#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/time.h>
#include <reliefnt/paging.h>
#undef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0
#include "../../kernel/reliefnt/kernel/ipc/futex.c"

static struct task tasks_test[4], *current;
static uint64_t ticks = 100;
static uint32_t words[1024] __attribute__((aligned(4096)));
static unsigned blocks, wakes;
static uint64_t readonly_address;
static struct linux_robust_list_head robust;
static struct { uint64_t next; uint32_t word; } robust_node;

struct task *sched_current_task(void) { return current; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
void sched_block_current(void) { current->state = TASK_BLOCKED; ++blocks; }
void sched_sleep_current_until(uint64_t deadline)
{ current->wake_tick = deadline; sched_block_current(); }
void sched_mark_ready(uint32_t pid)
{ assert(pid && pid <= 4); tasks_test[pid - 1].state = TASK_READY; ++wakes; }
uint64_t time_ticks(void) { return ticks; }
int time_wall_clock(struct reliefos_time_info *out)
{ out->unix_seconds = 1000; return 0; }
int time_clock_get(int32_t clock, struct linux_timespec *out)
{ assert(clock == LINUX_CLOCK_REALTIME); *out = (struct linux_timespec){1000, 700000000}; return 0; }
bool user_range_ok(uint64_t ptr, uint64_t len)
{ return ptr >= 4096 && ptr + len >= ptr; }
bool user_range_writable(uint64_t ptr, uint64_t len)
{ return user_range_ok(ptr, len) && ptr != readonly_address; }
bool address_space_user_page_writable(const struct address_space *as, uint64_t address)
{ (void)as; return user_range_ok(address, 4); }
bool address_space_user_page_readable(const struct address_space *as, uint64_t address)
{ (void)as; return user_range_ok(address, 4); }
bool address_space_handle_cow_fault(struct address_space *as, uint64_t address)
{ (void)as; (void)address; return false; }
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t address)
{ (void)as; return address & ~4095ULL; }

static long call(unsigned thread, unsigned word, int op, unsigned val,
                 uint64_t timeout, unsigned word2, unsigned val3)
{
    current = &tasks_test[thread];
    return syscall_futex((uintptr_t)&words[word], op, val, timeout,
                        (uintptr_t)&words[word2], val3);
}

int main(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        tasks_test[i].pid = i + 1;
        tasks_test[i].as.cr3 = i == 3 ? 8192 : 4096;
    }
    assert(call(0, 0, FUTEX_WAIT, 1, 0, 0, 0) == -EAGAIN && !blocks);
    assert(call(0, 0, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(call(1, 0, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(call(3, 0, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 9, 0, 0, 0) == 0);
    assert(call(2, 0, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0) == 1 && wakes == 1);
    words[0] = 99;
    assert(call(0, 0, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0) == 0);
    assert(call(2, 0, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG, 0, 1, 1, 98) == -EAGAIN);
    assert(call(2, 0, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG, 0, 1, 1, 99) == 1);
    assert(call(2, 0, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 9, 0, 0, 0) == 0);
    assert(call(2, 1, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 9, 0, 0, 0) == 1);
    assert(call(1, 0, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, 0, 0, 0) == 0);
    assert(call(0, 1, FUTEX_WAIT_BITSET, 0, 0, 0, 0) == -EINVAL);
    assert(call(0, 1, FUTEX_WAIT_BITSET, 0, 0, 0, 2) == KERNEL_SYSCALL_BLOCKED);
    assert(call(1, 1, FUTEX_WAKE_BITSET, 1, 0, 0, 1) == 0);
    assert(call(3, 1, FUTEX_WAKE_BITSET, 1, 0, 0, 2) == 1);
    assert(call(0, 1, FUTEX_WAIT_BITSET, 0, 0, 0, 2) == 0);
    int64_t ts[2] = {0, 10000000};
    assert(call(0, 1, FUTEX_WAIT, 0, (uintptr_t)ts, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    ++ticks;
    assert(call(1, 1, FUTEX_WAKE, 1, 0, 0, 0) == 0);
    assert(call(0, 1, FUTEX_WAIT, 0, (uintptr_t)ts, 0, 0) == -ETIMEDOUT);
    assert(call(0, 1, FUTEX_WAIT, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    tasks_test[1].shared_mm = &tasks_test[0].address_space;
    tasks_test[1].shared_mm->references = 2;
    tasks_test[1].clear_child_tid = (uintptr_t)&words[1];
    futex_task_exit(&tasks_test[1]);
    assert(call(0, 1, FUTEX_WAIT, 0, 0, 0, 0) == 0);
    assert(!waiters && !tasks_test[1].clear_child_tid);
    words[1] = 37;
    tasks_test[1].shared_mm->references = 1;
    tasks_test[1].clear_child_tid = (uintptr_t)&words[1];
    futex_task_exit(&tasks_test[1]);
    assert(words[1] == 37 && !tasks_test[1].clear_child_tid);
    words[1] = 0;
    tasks_test[1].shared_mm = NULL;
    robust = (struct linux_robust_list_head){(uintptr_t)&robust_node, 8, 0};
    robust_node.next = (uintptr_t)&robust;
    robust_node.word = tasks_test[1].pid | FUTEX_WAITERS;
    tasks_test[1].robust_list = (uintptr_t)&robust;
    futex_task_exit(&tasks_test[1]);
    assert(robust_node.word == (FUTEX_WAITERS | FUTEX_OWNER_DIED));
    robust.next = (uintptr_t)&robust;
    robust.list_op_pending = (uintptr_t)&robust_node;
    robust_node.word = tasks_test[1].pid;
    tasks_test[1].robust_list = (uintptr_t)&robust;
    futex_task_exit(&tasks_test[1]);
    assert(robust_node.word == FUTEX_OWNER_DIED);

    words[4] = 0;
    words[5] = 7;
    assert(call(0, 4, FUTEX_WAIT, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(call(1, 5, FUTEX_WAIT, 7, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(call(2, 4, FUTEX_WAKE_OP, 1, 1, 5,
                FUTEX_OP(FUTEX_OP_ADD, -3, FUTEX_OP_CMP_EQ, 7)) == 2);
    assert(words[5] == 4);
    assert(call(0, 4, FUTEX_WAIT, 0, 0, 0, 0) == 0);
    assert(call(1, 5, FUTEX_WAIT, 7, 0, 0, 0) == 0);
    assert(call(0, 5, FUTEX_WAIT, 4, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    /* Comparison uses the previous signed value, not the modified word. */
    assert(call(2, 4, FUTEX_WAKE_OP, 1, 1, 5,
                FUTEX_OP(FUTEX_OP_SET, -1, FUTEX_OP_CMP_LT, 0)) == 0);
    assert(words[5] == UINT32_MAX && tasks_test[0].futex_state == 1);
    assert(call(2, 4, FUTEX_WAKE_OP, 0, 0, 5,
                FUTEX_OP(FUTEX_OP_XOR | FUTEX_OP_OPARG_SHIFT, 33, FUTEX_OP_CMP_LT, 0)) == 1);
    assert(words[5] == (UINT32_MAX ^ 2));
    assert(call(0, 5, FUTEX_WAIT, 4, 0, 0, 0) == 0);
    words[5] = 7;
    assert(call(2, 4, FUTEX_WAKE_OP, 1, 1, 5, FUTEX_OP(7, 9, 0, 0)) == -ENOSYS);
    assert(words[5] == 7);
    assert(call(2, 4, FUTEX_WAKE_OP, 1, 1, 5, FUTEX_OP(FUTEX_OP_SET, 9, 7, 0)) == -ENOSYS);
    assert(words[5] == 9); /* Linux performs the atomic update before decoding cmp. */
    readonly_address = (uintptr_t)&words[5];
    assert(call(2, 4, FUTEX_WAKE_OP, 1, 1, 5, FUTEX_OP(0, 0, 0, 0)) == -EFAULT);
    assert(words[5] == 9);
    readonly_address = 0;
    for (int count = -1; count <= 0; ++count) {
        assert(call(0, 4, FUTEX_WAIT, 0, 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
        assert(call(2, 4, FUTEX_WAKE, count, 0, 0, 0) == 1);
        assert(call(0, 4, FUTEX_WAIT, 0, 0, 0, 0) == 0);
    }
    ts[0] = 1000; ts[1] = 750000000;
    assert(call(0, 4, FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 0,
                (uintptr_t)ts, 0, FUTEX_BITSET_MATCH_ANY) == KERNEL_SYSCALL_BLOCKED);
    assert(tasks_test[0].futex_deadline == ticks + 5);
    assert(call(2, 4, FUTEX_WAKE, 1, 0, 0, 0) == 1);
    assert(call(0, 4, FUTEX_WAIT_BITSET, 0, 0, 0, FUTEX_BITSET_MATCH_ANY) == 0);
    ts[0] = INT64_MAX; ts[1] = 999999999;
    assert(call(0, 4, FUTEX_WAIT, 0, (uintptr_t)ts, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    assert(tasks_test[0].futex_deadline > ticks);
    futex_cancel_wait(&tasks_test[0]);
    assert(!waiters);
    uint64_t address = (uintptr_t)&words[8];
    uint64_t destination = (uintptr_t)&words[9];
    current = &tasks_test[0];
    assert(syscall_futex_wait2(address, 0, 2, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE,
                                0, LINUX_CLOCK_MONOTONIC) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks_test[2];
    assert(syscall_futex_wake2(address, 2, 0, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE) == 0);
    assert(tasks_test[0].futex_state == 1);
    assert(syscall_futex_wake2(address, 1, 1, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE) == 0);
    assert(syscall_futex_wake2(address, 2, UINT64_MAX,
                                FUTEX2_SIZE_U32 | FUTEX2_PRIVATE | (1ULL << 32)) == 1);
    current = &tasks_test[0];
    assert(syscall_futex_wait2(address, 0, 2, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE,
                                0, LINUX_CLOCK_MONOTONIC) == 0);
    assert(syscall_futex_wait2(address, 1, 1, FUTEX2_SIZE_U32, 0, 99) == -EAGAIN);
    assert(syscall_futex_wait2(address, 1ULL << 32, 1, FUTEX2_SIZE_U32, 0, 0) == -EINVAL);
    assert(syscall_futex_wake2(address, 1ULL << 32, 0, FUTEX2_SIZE_U32) == -EINVAL);
    assert(syscall_futex_wake2(4, 1, 0, FUTEX2_SIZE_U32) == -EFAULT);
    assert(syscall_futex_requeue2(1, 0, 0, 0) == -EFAULT);
    assert(syscall_futex_requeue2(0, 0, 0, 0) == -EINVAL);
    struct futex_waitv vector[2] = {
        {.uaddr = address, .flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE},
        {.uaddr = destination, .flags = FUTEX2_SIZE_U32},
    };
    assert(syscall_futex_wait2(address, 0, 1, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE,
                                0, 0) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks_test[2];
    assert(syscall_futex_requeue2((uintptr_t)vector, 1ULL << 32, 0, (1ULL << 32) | 1) == 1);
    assert(syscall_futex_wake2(address, 1, 1, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE) == 0);
    current = &tasks_test[3];
    assert(syscall_futex_wake2(destination, 1, 1, FUTEX2_SIZE_U32) == 1);
    current = &tasks_test[0];
    assert(syscall_futex_wait2(address, 0, 1, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0, 0) == 0);
    ts[0] = 1000; ts[1] = 750000000;
    assert(syscall_futex_wait2(address, 0, 1, FUTEX2_SIZE_U32,
                                (uintptr_t)ts, 1ULL << 32) == KERNEL_SYSCALL_BLOCKED);
    ticks += 5;
    assert(syscall_futex_wait2(address, 0, 1, FUTEX2_SIZE_U32,
                                (uintptr_t)ts, 0) == -ETIMEDOUT);
    assert(!waiters);
    words[10] = 0;
    words[11] = 0;
    struct futex_waitv waitv[2] = {
        {.uaddr = (uintptr_t)&words[10], .val = 0,
         .flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE},
        {.uaddr = (uintptr_t)&words[11], .val = 0,
         .flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE},
    };
    current = &tasks_test[0];
    assert(syscall_futex_waitv((uintptr_t)waitv, 0, 0, 0, LINUX_CLOCK_MONOTONIC) == -EINVAL);
    int64_t waitv_timeout[2] = {0, 0};
    assert(syscall_futex_waitv((uintptr_t)waitv, 2, 0, (uintptr_t)waitv_timeout,
                               LINUX_CLOCK_MONOTONIC) == -ETIMEDOUT);
    assert(syscall_futex_waitv((uintptr_t)waitv, 2, 0, 0, LINUX_CLOCK_MONOTONIC) == KERNEL_SYSCALL_BLOCKED);
    current = &tasks_test[1];
    assert(syscall_futex_wake2((uintptr_t)&words[11], 1, 1,
                               FUTEX2_SIZE_U32 | FUTEX2_PRIVATE) == 1);
    current = &tasks_test[0];
    assert(syscall_futex_waitv((uintptr_t)waitv, 2, 0, 0, LINUX_CLOCK_MONOTONIC) == 1);
    assert(!waiters);
    puts("PASS futex: queues, requeue, wake-op, signed/shift operands, access errors, realtime and saturated deadlines, robust exit");
}
