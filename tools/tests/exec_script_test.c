#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/reliefnt/tools/tests/modules/syscall_dispatch.h"

void drm_file_close(struct task_file *file)
{ if (file) file->drm_file = NULL; }

static bool fail_allocation;
bool user_range_ok(uint64_t address, uint64_t size) { return address != 0 || size == 0; }
size_t user_strlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) ++n; return n; }
void *kernel_malloc(size_t size) { return fail_allocation ? NULL : malloc(size); }
void kernel_free(void *memory) { free(memory); }

int main(void)
{
    struct exec_params_kernel vectors = {0};
    char *arguments[1026];
    for (unsigned i = 0; i < 1025; ++i) arguments[i] = "-DCONFIG_TEST=1";
    arguments[128] = NULL;
    assert(copy_exec_params_from_user((uintptr_t)arguments, 0, &vectors) == 0);
    assert(vectors.argc == 128);
    for (unsigned i = 0; i < 1025; ++i) arguments[i] = "x";
    arguments[1024] = NULL;
    assert(copy_exec_params_from_user((uintptr_t)arguments, 0, &vectors) == 0);
    assert(vectors.argc == 1024 && !vectors.argv[1024]);
    arguments[1024] = "x";
    arguments[1025] = NULL;
    assert(copy_exec_params_from_user((uintptr_t)arguments, 0, &vectors) == -RELIEFOS_E2BIG);
    char *environment[130];
    for (unsigned i = 0; i < 129; ++i) environment[i] = "KEY=value";
    environment[128] = NULL;
    arguments[118] = NULL;
    assert(copy_exec_params_from_user((uintptr_t)arguments, (uintptr_t)environment, &vectors) == 0);
    assert(vectors.argc == 118 && vectors.envc == 128 && !vectors.envp[128]);
    environment[128] = "KEY=value";
    environment[129] = NULL;
    assert(copy_exec_params_from_user(0, (uintptr_t)environment, &vectors) == -RELIEFOS_E2BIG);
    char long_argument[12000];
    memset(long_argument, 'a', sizeof(long_argument) - 1);
    long_argument[sizeof(long_argument) - 1] = 0;
    char *long_vector[] = {long_argument, NULL};
    assert(copy_exec_params_from_user((uintptr_t)long_vector, 0, &vectors) == 0);
    assert(vectors.data_len == sizeof(long_argument));
    puts("PASS exec build vectors: 118/1024 args, 128 env, long string and count overflow");
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
    while (params.argc < SCHED_EXEC_ARG_MAX) assert(!exec_append_string(&params, "x", false));
    original = params;
    assert(exec_script_arguments(&params, "/bin/sh", "-e", "script") == -RELIEFOS_E2BIG);
    assert(!memcmp(&params, &original, sizeof(params)));
    puts("PASS production shebang parser, bounded fuzz, argv rewrite and allocation rollback");
}
