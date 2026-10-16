/* Detect wrong auxv capacity/layout and advertising an absent vDSO image. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/kernel/exec/userland.c"

static unsigned char stack[16 * 4096] __attribute__((aligned(4096)));
static const uint64_t top = 0x2ffff000;
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t address)
{
    (void)as;
    if (address < top - sizeof(stack) || address >= top) return 0;
    return (uint64_t)(uintptr_t)(stack + ((address - (top - sizeof(stack))) & ~4095ULL));
}

int main(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->stack_top = top;
    strcpy(task->path, "/probe");
    task->exec_argc = 1;
    strcpy(task->exec_data, "probe");
    task->exec_argv[0] = task->exec_data;
    task->exec_data_len = 6;
    task->dynamic_launch.main_entry = 0x400100;
    task->dynamic_launch.main_phdr = 0x400040;
    task->exec_phnum = 3;
    for (unsigned i = 0; i < 16; ++i) task->dynamic_launch.random[i] = (unsigned char)(i + 1);
    const uint64_t vdso = 0x28000000;
    assert(prepare_user_exec_stack(task, task->path, vdso) == 0);
    assert(!(task->frame.rsp & 15));
    uint64_t *words = (void *)(stack + task->frame.rsp - (top - sizeof(stack)));
    assert(words[0] == 1 && words[1] && words[2] == 0 && words[3] == 0);
    uint64_t *aux = words + 4;
    unsigned count = 0, seen_vdso = 0, seen_random = 0;
    for (; aux[0]; aux += 2, ++count) {
        assert(count < 17);
        if (aux[0] == 33) { assert(aux[1] == vdso); ++seen_vdso; }
        if (aux[0] == 25) {
            const unsigned char *random = stack + aux[1] - (top - sizeof(stack));
            for (unsigned i = 0; i < 16; ++i) assert(random[i] == i + 1);
            ++seen_random;
        }
    }
    assert(count == 16 && aux[1] == 0 && seen_vdso == 1 && seen_random == 1);
    free(task);
    puts("PASS initial stack: vDSO auxv, terminator, random data and SysV alignment");
    return 0;
}
