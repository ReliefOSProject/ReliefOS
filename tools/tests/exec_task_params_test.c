/* Exercise production task exec-buffer ownership, including allocation rollback. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/sched/exec.c"

static unsigned allocations, fail_at, live;
void *kernel_malloc(size_t size)
{
    if (++allocations == fail_at) return NULL;
    void *ptr = malloc(size);
    if (ptr) ++live;
    return ptr;
}
void kernel_free(void *ptr) { if (ptr) { assert(live); --live; free(ptr); } }

int main(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    char data[] = "first\0second\0K=value";
    char *argv[] = {data, data + 6, NULL}, *envp[] = {data + 13, NULL};
    assert(!sched_copy_task_exec_params(task, 2, argv, 1, envp, data, sizeof(data)));
    assert(live == 3 && task->exec_argv[1] == task->exec_data + 6);
    assert(task->exec_envp[0] == task->exec_data + 13);
    assert(!strcmp(task->exec_envp[0], "K=value"));
    assert(!task->exec_argv[2] && !task->exec_envp[1]);
    char *old = task->exec_data;
    for (unsigned i = 1; i <= 3; ++i) {
        fail_at = allocations + i;
        assert(sched_copy_task_exec_params(task, 2, argv, 1, envp, data, sizeof(data)) == -12);
        assert(task->exec_data == old && live == 3);
    }
    fail_at = 0;
    char *bad[] = {data + sizeof(data), NULL};
    assert(sched_copy_task_exec_params(task, 1, bad, 0, NULL, data, sizeof(data)) == -22);
    assert(task->exec_data == old && live == 3);
    /* fork can inherit metadata even after a caller lowers its stack limit. */
    task->limits.stack.rlim_cur = 1;
    for (unsigned i = 0; i < 100; ++i) {
        assert(!sched_copy_task_exec_params(task, task->exec_argc, task->exec_argv,
                                           task->exec_envc, task->exec_envp,
                                           task->exec_data, task->exec_data_len));
        assert(live == 3 && !strcmp(task->exec_argv[1], "second"));
    }
    kernel_free(task->exec_argv); kernel_free(task->exec_envp); kernel_free(task->exec_data);
    assert(!live);
    free(task);
    puts("PASS exec task ownership: rebinding, replacement, all allocation failures and fork metadata");
}
