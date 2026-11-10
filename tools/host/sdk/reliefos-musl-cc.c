/* Relocatable SDK driver. Arguments are passed to execvp, never a shell. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char *join(const char *a, const char *b)
{
    size_t x = strlen(a), y = strlen(b);
    if (x > SIZE_MAX - y - 1) { errno = ENOMEM; return NULL; }
    char *p = malloc(x + y + 1);
    if (p) { memcpy(p, a, x); memcpy(p+x, b, y+1); }
    return p;
}

static char *sdk_root(void)
{
    size_t capacity = 256;
    for (;;) {
        char *p = malloc(capacity);
        if (!p) return NULL;
        ssize_t n = readlink("/proc/self/exe", p, capacity - 1);
        if (n < 0) { free(p); return NULL; }
        if ((size_t)n < capacity - 1) {
            p[n] = 0;
            char *last = strrchr(p, '/');
            if (last) *last = 0;
            last = strrchr(p, '/');
            if (!last || strcmp(last, "/bin")) { free(p); errno = EINVAL; return NULL; }
            *last = 0;
            return p;
        }
        free(p);
        if (capacity > SIZE_MAX / 2) { errno = ENOMEM; return NULL; }
        capacity *= 2;
    }
}

/* The compiler's single-line resource directory is data, even with spaces. */
static char *resource_dir(const char *compiler)
{
    int fds[2];
    if (pipe(fds)) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (!pid) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        if (fds[1] != STDOUT_FILENO) close(fds[1]);
        char *const args[] = {(char *)compiler, "-print-resource-dir", NULL};
        execvp(compiler, args);
        _exit(errno == ENOENT ? 127 : 126);
    }
    close(fds[1]);
    size_t size = 0, cap = 256;
    char *data = malloc(cap);
    int failed = !data;
    while (!failed) {
        if (size + 1 == cap) {
            /* A resource directory cannot reasonably be a megabyte. */
            if (cap >= 1024*1024) { failed = 1; break; }
            char *next = realloc(data, cap*2);
            if (!next) { failed = 1; break; }
            data = next; cap *= 2;
        }
        ssize_t n = read(fds[0], data+size, cap-size-1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { failed = 1; break; }
        if (!n) break;
        size += (size_t)n;
    }
    close(fds[0]);
    int status;
    pid_t reaped;
    do { reaped = waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    if (reaped < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    if (!failed) {
        while (size && (data[size-1] == '\n' || data[size-1] == '\r')) --size;
        if (!size || memchr(data, 0, size) || memchr(data, '\n', size)) failed = 1;
    }
    if (failed) { free(data); errno = EIO; return NULL; }
    data[size] = 0;
    return data;
}

int main(int argc, char **argv)
{
    const char *compiler = getenv("RELIEFOS_CC");
    if (!compiler || !*compiler) compiler = getenv("LEONOS_CC");
    if (!compiler || !*compiler) compiler = "clang";
    const char *abi = getenv("RELIEFOS_SDK_ABI");
    int leonos_abi = abi && strcmp(abi, "leonos") == 0;
    if (abi && *abi && !leonos_abi && strcmp(abi, "reliefos") != 0) {
        fprintf(stderr, "reliefos-musl-cc: RELIEFOS_SDK_ABI must be reliefos or leonos\n");
        return 2;
    }
    char **args = calloc((size_t)argc + 64, sizeof(*args));
    if (!args) { perror("SDK driver"); return 126; }
    size_t n = 0;
    args[n++] = (char *)compiler;
    args[n++] = "--target=x86_64-linux-musl";
    int info = 0, only = 0, shared = 0, static_link = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a,"--version") || !strcmp(a,"-dumpmachine") ||
            !strcmp(a,"-dumpversion") || !strcmp(a,"-print-resource-dir")) info = 1;
        if (!strcmp(a,"-c") || !strcmp(a,"-S") || !strcmp(a,"-E") ||
            !strcmp(a,"-fsyntax-only") || !strcmp(a,"-r")) only = 1;
        if (!strcmp(a,"-shared")) shared = 1;
        if (!strcmp(a,"-static")) static_link = 1;
    }
    char *paths[9] = {0}, *sdk = NULL, *resource = NULL;
    int result = 126;
    if (!info) {
        sdk = sdk_root(); resource = resource_dir(compiler);
        if (!sdk || !resource) goto failure;
        const char *suffixes[] = {"/include", "/lib/crt1.o", "/lib/Scrt1.o", "/lib/crti.o", "/lib/crtn.o", "/lib/mimalloc.o", "/lib"};
        for (unsigned i = 0; i < 7; ++i) {
            paths[i] = join(sdk, suffixes[i]);
            if (!paths[i]) goto failure;
        }
        paths[7] = join(resource, "/include");
        paths[8] = join("-L", paths[6]);
        if (!paths[7] || !paths[8]) goto failure;
        args[n++] = "-fuse-ld=lld"; args[n++] = "-nostdlib"; args[n++] = "-nostdinc";
        args[n++] = "-isystem"; args[n++] = paths[0];
        args[n++] = "-isystem"; args[n++] = paths[7];
        args[n++] = "-D_GNU_SOURCE"; args[n++] = "-DRELIEFOS_USE_MUSL";
        args[n++] = "-mno-avx"; args[n++] = "-mno-avx2";
        if (!only && !shared) {
            args[n++] = static_link ? paths[1] : paths[2]; args[n++] = paths[3];
            if (static_link) args[n++] = "-Wl,--image-base=0x4000000";
            else {
                args[n++] = "-pie";
                args[n++] = "-Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1";
                args[n++] = leonos_abi
                    ? "-Wl,-rpath,/usr/lib/leonos:/lib:/usr/lib"
                    : "-Wl,-rpath,/usr/lib/reliefos:/lib:/usr/lib";
            }
        }
    }
    for (int i = 1; i < argc; ++i) args[n++] = argv[i];
    if (!info && !only) {
        args[n++] = "-x"; args[n++] = "none"; args[n++] = paths[8];
        if (static_link) {
            args[n++] = paths[5]; args[n++] = "-Wl,--start-group";
            args[n++] = leonos_abi ? "-lleonos" : "-lreliefos";
            args[n++] = "-lpng"; args[n++] = "-lz";
            args[n++] = "-lmbedtls"; args[n++] = "-lmbedx509";
            args[n++] = "-lmbedcrypto"; args[n++] = "-lcrypt";
            args[n++] = "-lc"; args[n++] = "-Wl,--end-group";
        } else {
            args[n++] = "-l:libmimalloc.so.3";
            args[n++] = leonos_abi ? "-l:libleonos.so.2" : "-l:libreliefos.so.2";
            args[n++] = "-Wl,-rpath-link"; args[n++] = paths[6];
            args[n++] = "-lpng"; args[n++] = "-lz";
            args[n++] = "-lmbedtls"; args[n++] = "-lmbedx509";
            args[n++] = "-lmbedcrypto"; args[n++] = "-lc";
        }
        args[n++] = "-l:libclang_rt.builtins.a";
        if (!shared) args[n++] = paths[4];
    }
    execvp(compiler, args);
    result = errno == ENOENT ? 127 : 126;
failure:
    perror("reliefos-musl-cc");
    free(args); free(sdk); free(resource);
    for (unsigned i = 0; i < 9; ++i) free(paths[i]);
    return result;
}
