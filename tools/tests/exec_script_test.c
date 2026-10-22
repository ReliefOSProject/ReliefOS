#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/tools/tests/modules/syscall_dispatch.h"

void drm_file_close(struct task_file *file)
{ if (file) file->drm_file = NULL; }

static bool fail_allocation;
static struct task caller;
struct task *sched_current_task(void) { return &caller; }
bool user_range_ok(uint64_t address, uint64_t size) { return address >= 4096 && size <= UINT64_MAX - address; }
size_t user_strlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) ++n; return n; }
void *kernel_malloc(size_t size) { return fail_allocation ? NULL : malloc(size); }
void kernel_free(void *memory) { free(memory); }

int main(void)
{
    caller.limits.stack.rlim_cur = 8u * 1024u * 1024u;
    assert(sched_exec_argument_limit(0) == 131072);
    assert(sched_exec_argument_limit(8u * 1024u * 1024u) == 2097152);
    assert(sched_exec_argument_limit(UINT64_MAX) == 6291456);
    struct exec_params_kernel vectors = {0};
    char *environment[492];
    for (unsigned i = 0; i < 491; ++i) environment[i] = "KEY=compiler-flags-and-paths";
    environment[491] = NULL;
    assert(!copy_exec_params_from_user(0, (uintptr_t)environment, &vectors));
    assert(vectors.envc == 491 && !vectors.envp[491]);
    assert(vectors.data_cap == vectors.data_len && vectors.data_cap < 32768);
    free_exec_params(&vectors);
    char *arguments[2049];
    for (unsigned i = 0; i < 2048; ++i) arguments[i] = "-DCONFIG_TEST=1";
    arguments[2048] = NULL;
    assert(!copy_exec_params_from_user((uintptr_t)arguments, (uintptr_t)environment, &vectors));
    assert(vectors.argc == 2048 && vectors.envc == 491);
    free_exec_params(&vectors);
    unsigned char unaligned[2 * sizeof(uint64_t) + 1] = {0};
    uint64_t pointer = (uintptr_t)"unaligned";
    memcpy(unaligned + 1, &pointer, sizeof(pointer));
    assert(!copy_exec_params_from_user((uintptr_t)(unaligned + 1), 0, &vectors));
    assert(vectors.argc == 1 && !strcmp(vectors.argv[0], "unaligned"));
    free_exec_params(&vectors);
    char *long_argument = malloc(SCHED_EXEC_STR_MAX + 1);
    memset(long_argument, 'a', SCHED_EXEC_STR_MAX);
    long_argument[SCHED_EXEC_STR_MAX - 1] = 0;
    char *long_vector[] = {long_argument, NULL};
    assert(!copy_exec_params_from_user((uintptr_t)long_vector, 0, &vectors));
    assert(vectors.data_len == SCHED_EXEC_STR_MAX);
    free_exec_params(&vectors);
    long_argument[SCHED_EXEC_STR_MAX - 1] = 'a';
    long_argument[SCHED_EXEC_STR_MAX] = 0;
    assert(copy_exec_params_from_user((uintptr_t)long_vector, 0, &vectors) == -RELIEFOS_E2BIG);
    long_argument[65535] = 0;
    char *many[40];
    for (unsigned i = 0; i < 39; ++i) many[i] = long_argument;
    many[39] = NULL;
    assert(copy_exec_params_from_user(0, (uintptr_t)many, &vectors) == -RELIEFOS_E2BIG);
    caller.limits.stack.rlim_cur = UINT64_MAX;
    assert(!copy_exec_params_from_user(0, (uintptr_t)many, &vectors));
    free_exec_params(&vectors);
    caller.limits.stack.rlim_cur = 64 * 1024;
    many[1] = NULL;
    assert(!copy_exec_params_from_user(0, (uintptr_t)many, &vectors));
    free_exec_params(&vectors);
    many[1] = long_argument; many[2] = NULL;
    assert(copy_exec_params_from_user(0, (uintptr_t)many, &vectors) == -RELIEFOS_E2BIG);
    caller.limits.stack.rlim_cur = 8u * 1024u * 1024u;
    free(long_argument);
    assert(copy_exec_params_from_user(1, 0, &vectors) == -RELIEFOS_EFAULT);
    char *invalid[] = {(char *)1, NULL};
    assert(copy_exec_params_from_user((uintptr_t)invalid, 0, &vectors) == -RELIEFOS_EFAULT);
    fail_allocation = true;
    assert(copy_exec_params_from_user(0, (uintptr_t)environment, &vectors) == -RELIEFOS_ENOMEM);
    assert(!vectors.argv && !vectors.envp && !vectors.data);
    fail_allocation = false;
    puts("PASS Linux exec budgets: 491 env, 2048 args, string/total/stack limits and allocation rollback");
    char header[256], *name, *argument;
    memset(header, 0, sizeof(header));
    strcpy(header, "#! \t/bin/sh \targ one \t\n");
    assert(script_header(header, &name, &argument) == 1);
    assert(!strcmp(name, "/bin/sh") && !strcmp(argument, "arg one"));
    memset(header, 'a', sizeof(header));
    header[0] = '#'; header[1] = '!';
    assert(script_header(header, &name, &argument) == -LINUX_ENOEXEC);
    memset(header + 2, ' ', sizeof(header) - 2);
    assert(script_header(header, &name, &argument) == -LINUX_ENOEXEC);
    memset(header, 0, sizeof(header));
    strcpy(header, "#!\n");
    assert(script_header(header, &name, &argument) == -LINUX_ENOEXEC);
    strcpy(header, "#!");
    assert(script_header(header, &name, &argument) == 1 && !name[0] && !argument);
    strcpy(header, "#!/bin/sh\r\n");
    assert(script_header(header, &name, &argument) == 1 && !strcmp(name, "/bin/sh\r"));
    memset(header, 'a', sizeof(header));
    memcpy(header, "#!/bin/sh ", 10);
    assert(script_header(header, &name, &argument) == 1 && !strcmp(name, "/bin/sh"));
    assert(strlen(argument) == 245);
    uint32_t random = 871;
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        for (unsigned i = 0; i < sizeof(header); ++i) {
            random = random * 1664525u + 1013904223u;
            header[i] = (char)(random >> 24);
        }
        header[0] = '#'; header[1] = '!';
        if (script_header(header, &name, &argument) == 1) {
            assert(name >= header + 2 && name < header + sizeof(header));
            assert(strlen(name) < sizeof(header));
            if (argument) assert(argument > name && strlen(argument) < sizeof(header));
        }
    }
    struct exec_params_kernel params = {0};
    assert(!allocate_exec_params(&params, 2, 1, 131072, 64));
    assert(!exec_append_string(&params, "old argv0", false));
    assert(!exec_append_string(&params, "tail", false));
    assert(!exec_append_string(&params, "NAME=value", true));
    struct exec_params_kernel original = params;
    fail_allocation = true;
    assert(exec_script_arguments(&params, "/bin/sh", "-e", "script") == -RELIEFOS_ENOMEM);
    assert(!memcmp(&params, &original, sizeof(params)));
    fail_allocation = false;
    assert(!exec_script_arguments(&params, "/bin/sh", "arg one", "script"));
    assert(params.argc == 4 && params.envc == 1);
    assert(!strcmp(params.argv[0], "/bin/sh") && !strcmp(params.argv[1], "arg one"));
    assert(!strcmp(params.argv[2], "script") && !strcmp(params.argv[3], "tail"));
    assert(!params.argv[4] && !strcmp(params.envp[0], "NAME=value") && !params.envp[1]);
    free_exec_params(&params);
    puts("PASS production shebang parser, bounded fuzz, argv rewrite and allocation rollback");
}
