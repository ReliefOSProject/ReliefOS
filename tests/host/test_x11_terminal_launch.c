#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>

static jmp_buf launched;
static int forks;
static pid_t capture_fork(void);
static int capture_execve(const char *, char *const[], char *const[]);
#define fork capture_fork
#define execve capture_execve
#include "../../userland/runtime/src/launch.c"
#undef fork
#undef execve

int reliefos_app_registry_resolve(const char *id, char *path, uint32_t capacity)
{ (void)path; (void)capacity; assert(!strcmp(id, "terminal")); return -1; }
int reliefos_environment_build(char *const overrides[], char ***out)
{ (void)overrides; *out = NULL; return 0; }
int reliefos_session_apply(void) { assert(0); return -1; }

static pid_t capture_fork(void) { ++forks; return 0; }
static int capture_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)envp;
    assert(!strcmp(path, "/usr/bin/st"));
    assert(!strcmp(argv[0], "/usr/bin/st") && !strcmp(argv[1], "-e"));
    assert(!strcmp(argv[2], "/usr/bin/printf"));
    assert(!strcmp(argv[3], "space ; $(literal)") && !argv[4]);
    longjmp(launched, 1);
}

int main(void)
{
    char *args[] = {"/usr/bin/printf", "space ; $(literal)", NULL};
    if (!setjmp(launched)) { launch_in_terminal(args); assert(0); }
    assert(forks == 1);
    char *too_many[RELIEFOS_LAUNCH_MAX_ARGS + 2];
    for (unsigned i = 0; i <= RELIEFOS_LAUNCH_MAX_ARGS; ++i) too_many[i] = "x";
    too_many[RELIEFOS_LAUNCH_MAX_ARGS + 1] = NULL;
    errno = 0;
    assert(launch_in_terminal(too_many) == RELIEFOS_LAUNCH_ERR_TOO_MANY_ARGS);
    assert(errno == E2BIG && forks == 1);
    return 0;
}
