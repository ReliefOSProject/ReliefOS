/* Host-only behavioral fixture. Compile with clang (userland.c uses its
 * constant-array folding extension), ASan/UBSan and --gc-sections. Source
 * overrides allow the same executable contracts to run against pre-fix code. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TASK_EXIT_SCHED_SOURCE
#define TASK_EXIT_SCHED_SOURCE "../../kernel/reliefnt/kernel/reliefnt/sched/sched.c"
#endif
#ifndef TASK_EXIT_USERLAND_SOURCE
#define TASK_EXIT_USERLAND_SOURCE "../../kernel/reliefnt/kernel/exec/userland.c"
#endif
#include TASK_EXIT_SCHED_SOURCE
#include TASK_EXIT_USERLAND_SOURCE

static unsigned scheduler_lock_depth;
static unsigned execution_lock_depth;
static unsigned file_releases;
static unsigned io_drains;
static unsigned failures;
static bool interrupts_enabled = true;
static unsigned console_calls_with_irqs_disabled;
static void (*after_scheduler_unlock)(void);
static unsigned console_calls;
static unsigned console_calls_while_locked;
static char console_log[4096];
static size_t console_log_len;

uint32_t smp_current_cpu(void) { return 0; }
uint32_t smp_cpu_count(void) { return 1; }
bool smp_cpu_online(uint32_t cpu) { return cpu == 0; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(cr3 == 0x1000); }
uint64_t time_uptime_us(void) { return 0; }
uint64_t time_uptime_ms(void) { return 0; }

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    assert(lock == &scheduler_lock);
    assert(scheduler_lock_depth == 0);
    scheduler_lock_depth = 1;
    *flags = interrupts_enabled;
    interrupts_enabled = false;
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    assert(lock == &scheduler_lock);
    assert(scheduler_lock_depth == 1);
    scheduler_lock_depth = 0;
    interrupts_enabled = flags != 0;
    if (after_scheduler_unlock) {
        void (*callback)(void) = after_scheduler_unlock;
        after_scheduler_unlock = NULL;
        callback();
    }
}

void console_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ++console_calls;
    if (scheduler_lock_depth) ++console_calls_while_locked;
    if (!interrupts_enabled) ++console_calls_with_irqs_disabled;
    if (console_log_len < sizeof(console_log)) {
        int written = vsnprintf(console_log + console_log_len,
                                sizeof(console_log) - console_log_len,
                                format, args);
        if (written > 0) {
            size_t available = sizeof(console_log) - console_log_len - 1;
            console_log_len += (size_t)written < available ? (size_t)written : available;
        }
    }
    va_end(args);
}

int kernel_signal_fatal_pending(const struct task *task) { (void)task; return 0; }
void *kernel_malloc(size_t size) { return calloc(1, size); }
void kernel_free(void *ptr) { free(ptr); }
int storage_inode_put(struct storage_inode_ref *reference) { (void)reference; return 0; }
void task_groups_release(struct task *task) { (void)task; }
__attribute__((noreturn)) void bugcheck_panic(const char *message)
{
    (void)message;
    abort();
}
void kernel_execution_lock_irqsave(uint64_t *flags)
{
    assert(!scheduler_lock_depth && !execution_lock_depth);
    ++execution_lock_depth;
    *flags = interrupts_enabled;
    interrupts_enabled = false;
}
void kernel_execution_unlock_irqrestore(uint64_t flags)
{
    assert(execution_lock_depth == 1);
    --execution_lock_depth;
    interrupts_enabled = flags != 0;
}
void storage_drain_task_io(uint32_t pid)
{ (void)pid; assert(!scheduler_lock_depth); ++io_drains; }
void syscall_record_lock_cancel(struct task *task) { (void)task; }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; (void)task; }
uint64_t task_socket_cancel_receive(struct task *task) { (void)task; return 0; }
void task_release_syscall_file(struct task *task) { (void)task; }
void task_sysv_sem_exit(struct task *task) { (void)task; }
void futex_task_exit(struct task *task) { (void)task; }
void pty_process_session_exit(uint32_t tgid) { (void)tgid; }
void syscall_release_task_files(struct task *task)
{ (void)task; assert(!scheduler_lock_depth); ++file_releases; }
void address_space_destroy(struct address_space *as) { (void)as; }
void svga_gpu_release_owner(uint32_t owner) { (void)owner; }
void pty_process_exit(uint32_t pid) { (void)pid; }
void kernel_signal_flush(struct task *task, bool process, uint64_t mask)
{ (void)task; (void)process; (void)mask; }
int kernel_signal_enqueue(struct task *task, bool process, int sig,
                          const struct linux_siginfo *info)
{ (void)task; (void)process; (void)sig; (void)info; return 0; }
int kernel_signal_queue_task(struct task *task, int signal_number)
{ (void)task; (void)signal_number; return 0; }
int kernel_signal_queue_task_info(struct task *task, int sig,
                                  const struct linux_siginfo *info)
{ (void)task; (void)sig; (void)info; return 0; }
bool kernel_signal_default_ignored(int sig) { (void)sig; return false; }
void task_sysv_sem_stop(struct task *task) { (void)task; }

static void reset_console_log(void)
{
    console_calls = 0;
    console_calls_while_locked = 0;
    console_calls_with_irqs_disabled = 0;
    console_log_len = 0;
    console_log[0] = 0;
}

static struct task *fixture_task(uint32_t pid, const char *name)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = pid;
    task->tgid = pid;
    task->parent_pid = 0;
    task->process_group = pid;
    task->process_session = pid;
    task->kind = TASK_KIND_USER;
    task->state = TASK_READY;
    task->running_cpu = SCHED_CPU_NONE;
    task_copy_name(task, name);
    task->parent_exit_signal = 17;
    task->flags = TASK_FLAG_WAITABLE_CHILD;
    return task;
}

static void install_tasks(struct task **table, uint32_t count)
{
    tasks = table;
    task_count = count;
    task_capacity = count;
    current_pid[0] = 0;
}

static void check(bool condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", description);
        ++failures;
    }
}

static void check_log(const char *description, unsigned expected_calls)
{
    printf("%s calls=%u held=%u irq_disabled=%u\n", description, console_calls,
           console_calls_while_locked, console_calls_with_irqs_disabled);
    check(console_calls == expected_calls, description);
    check(console_calls_while_locked == 0, "console must run after scheduler unlock");
    check(console_calls_with_irqs_disabled == 0, "restore saved enabled IRQ state before console");
    assert(interrupts_enabled);
    assert(!scheduler_lock_depth && !execution_lock_depth);
}

static void check_one_exit_log(uint32_t pid, uint64_t code, const char *name)
{
    char expected[160];
    snprintf(expected, sizeof(expected),
             "scheduler task exited pid=%u name=%s code=%llu",
             pid, name, (unsigned long long)code);
    check(strstr(console_log, expected) != NULL, "exit snapshot pid/name/code");
}

static struct task *snapshot_task;
static void mutate_after_unlock(void)
{
    snapshot_task->pid = 141;
    snapshot_task->exit_code = 99;
    task_copy_name(snapshot_task, "changed-after-unlock");
}

static void repeat_after_unlock(void)
{
    sched_exit(snapshot_task->pid, 24);
}

int main(void)
{
    struct task *exiting = fixture_task(40, "fixture-exit");
    struct task *exit_table[] = {exiting};
    install_tasks(exit_table, 1);
    current_pid[0] = exiting->pid;

    reset_console_log();
    userland_process_exit(23);
    assert(exiting->state == TASK_EXITED);
    check_log("userland first exit", 1);
    check_one_exit_log(40, 23, "fixture-exit");
    assert(exiting->exit_code == 23 && exiting->running_cpu == SCHED_CPU_NONE);
    assert(exiting->flags & TASK_FLAG_RESOURCES_RELEASED);
    assert(file_releases == 1 && io_drains == 1);

    reset_console_log();
    userland_process_exit(24);
    sched_exit(40, 25);
    check_log("userland and scheduler repeated exit", 0);
    /* Logging idempotency must not change the existing status-update contract. */
    assert(exiting->exit_code == 25 && file_releases == 1 && io_drains == 1);

    reset_console_log();
    sched_exit(404, 0);
    check_log("unknown pid", 0);

    struct task *remote = fixture_task(41, "remote-exit");
    struct task *remote_table[] = {remote};
    install_tasks(remote_table, 1);
    remote->running_cpu = 1;
    snapshot_task = remote;
    after_scheduler_unlock = mutate_after_unlock;
    reset_console_log();
    sched_exit(41, UINT64_MAX);
    check_log("scheduler remote exit snapshot", 1);
    check_one_exit_log(41, UINT64_MAX, "remote-exit");
    assert(remote->state == TASK_EXITED && remote->running_cpu == 1);
    assert(!(remote->flags & TASK_FLAG_RESOURCES_RELEASED));
    assert(file_releases == 1 && io_drains == 1);

    /* An already-exited remote task must still clean up after CPU retirement. */
    remote->running_cpu = SCHED_CPU_NONE;
    reset_console_log();
    sched_exit(remote->pid, 26);
    check_log("remote repeated exit after retirement", 0);
    assert(remote->flags & TASK_FLAG_RESOURCES_RELEASED);
    assert(remote->exit_code == 26 && file_releases == 2 && io_drains == 2);

    struct task *nested = fixture_task(42, "nested-exit");
    struct task *nested_table[] = {nested};
    install_tasks(nested_table, 1);
    snapshot_task = nested;
    after_scheduler_unlock = repeat_after_unlock;
    reset_console_log();
    sched_exit(42, 23);
    check_log("exit repeated at unlock boundary", 1);
    check_one_exit_log(42, 23, "nested-exit");
    assert(nested->exit_code == 24 && file_releases == 3 && io_drains == 3);

    struct task *bounded = fixture_task(43, "bounded-exit");
    struct task *bounded_table[] = {bounded};
    char long_name[128];
    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = 0;
    bounded->name = long_name;
    install_tasks(bounded_table, 1);
    reset_console_log();
    sched_exit(43, 27);
    check_log("bounded exit name", 1);
    char truncated[SCHED_TASK_NAME_LEN];
    memset(truncated, 'x', sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = 0;
    char expected[160];
    snprintf(expected, sizeof(expected),
             "[reliefnt] scheduler task exited pid=43 name=%s code=27\n", truncated);
    check(strcmp(console_log, expected) == 0, "bounded NUL-terminated exit name");

    struct task *waiter = fixture_task(50, "fixture-waiter");
    struct task *child = fixture_task(51, "fixture-child");
    child->parent_pid = waiter->pid;
    child->parent_exit_signal = 17;
    child->state = TASK_EXITED;
    child->exit_code = 7;
    struct task *wait_table[] = {waiter, child};
    install_tasks(wait_table, 2);

    const uint32_t child_pid = child->pid;
    unsigned releases_before_wait = file_releases;
    int status = -1;
    struct linux_siginfo info;
    reset_console_log();
    assert(sched_wait_reap(waiter->pid, child_pid, 4 | 0x01000000u, &status, &info)
           == child_pid);
    check_log("WNOWAIT", 0);
    assert(child->pid == child_pid && status == (7 << 8));
    assert(info.fields.child.pid == (int32_t)child_pid && info.fields.child.status == 7);
    assert(file_releases == releases_before_wait);

    reset_console_log();
    int64_t reap_result = sched_wait_reap(waiter->pid, child_pid, 4, &status, &info);
    assert(reap_result == child_pid && child->pid == 0);
    assert(status == (7 << 8));
    assert(info.fields.child.pid == (int32_t)child_pid && info.fields.child.status == 7);
    check_log("wait reaped", 1);
    check(strstr(console_log, "scheduler wait reaped pid=51 by pid=50") != NULL,
          "reap log retains consumed pid");
    assert(file_releases == releases_before_wait + 1);

    reset_console_log();
    status = -1;
    assert(sched_wait_reap(waiter->pid, child_pid, 4, &status, NULL) == 0);
    assert(status == -1);
    check_log("already reaped", 0);
    assert(sched_wait_reap(999, -1, 4, NULL, NULL) == -2);

    child->pid = child_pid;
    child->parent_pid = waiter->pid;
    child->parent_exit_signal = 17;
    child->state = TASK_READY;
    reset_console_log();
    assert(sched_wait_reap(waiter->pid, child_pid, 4, NULL, NULL) == -RELIEFOS_EAGAIN);
    child->state = TASK_STOPPED;
    child->child_event = TASK_CHILD_EVENT_STOPPED;
    child->stop_signal = 19;
    assert(sched_wait_reap(waiter->pid, child_pid, 2, &status, NULL) == child_pid);
    assert(status == ((19 << 8) | 0x7f) && child->child_event == TASK_CHILD_EVENT_NONE);
    child->state = TASK_READY;
    child->child_event = TASK_CHILD_EVENT_CONTINUED;
    assert(sched_wait_reap(waiter->pid, child_pid, 8, &status, NULL) == child_pid);
    assert(status == 0xffff && child->child_event == TASK_CHILD_EVENT_NONE);
    check_log("live/stopped/continued waits", 0);

    /* Unlock must restore rather than unconditionally enable an IRQ-disabled
     * caller. The contract excludes scheduler-lock-induced console latency,
     * not interrupt suppression that the caller already owned. */
    struct task *irq_disabled = fixture_task(52, "irq-disabled-caller");
    struct task *irq_table[] = {irq_disabled};
    install_tasks(irq_table, 1);
    reset_console_log();
    interrupts_enabled = false;
    sched_exit(52, 0);
    check(console_calls == 1 && !console_calls_while_locked &&
          console_calls_with_irqs_disabled == 1, "IRQ-disabled caller console after unlock");
    assert(!interrupts_enabled && !scheduler_lock_depth && !execution_lock_depth);
    interrupts_enabled = true;
    printf("IRQ-disabled caller preserved: calls=%u held=%u irq_disabled=%u\n",
           console_calls, console_calls_while_locked, console_calls_with_irqs_disabled);

    free(exiting);
    free(remote);
    free(nested);
    free(bounded);
    free(waiter);
    free(child);
    free(irq_disabled);
    tasks = NULL;
    task_count = 0;

    printf("task-exit logging failures=%u\n", failures);
    return failures ? 1 : 0;
}
