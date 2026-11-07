#include <reliefos/stdio.h>
#include <reliefos/syscall.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <reliefos/layout.h>

#define STRESS_COUNT 96

static int fail(const char *step, int code)
{
    printf("[memtest.elf] FAIL %s code=%d\n", step, code);
    return 1;
}

static void fill_pattern(unsigned char *ptr, int len, int seed)
{
    for (int i = 0; i < len; ++i) {
        ptr[i] = (unsigned char)(seed + i * 13);
    }
}

static int check_pattern(const unsigned char *ptr, int len, int seed)
{
    for (int i = 0; i < len; ++i) {
        if (ptr[i] != (unsigned char)(seed + i * 13)) {
            return i + 1;
        }
    }
    return 0;
}

static int run_heap_stress(void)
{
    void *ptrs[STRESS_COUNT];
    int sizes[STRESS_COUNT];

    for (int i = 0; i < STRESS_COUNT; ++i) {
        sizes[i] = ((i * 37) % 701) + 1;
        ptrs[i] = malloc((size_t)sizes[i]);
        if (!ptrs[i]) {
            return fail("stress-alloc", i);
        }
        if (((unsigned long)ptrs[i] & 15UL) != 0) {
            return fail("stress-align", i);
        }
        fill_pattern((unsigned char *)ptrs[i], sizes[i], i + 1);
    }
    for (int i = 0; i < STRESS_COUNT; ++i) {
        int bad = check_pattern((const unsigned char *)ptrs[i], sizes[i], i + 1);
        if (bad) {
            return fail("stress-check-a", i);
        }
    }
    for (int i = 1; i < STRESS_COUNT; i += 2) {
        free(ptrs[i]);
        ptrs[i] = 0;
    }
    for (int i = 1; i < STRESS_COUNT; i += 2) {
        sizes[i] = ((i * 19) % 257) + 8;
        ptrs[i] = malloc((size_t)sizes[i]);
        if (!ptrs[i]) {
            return fail("stress-realloc", i);
        }
        if (((unsigned long)ptrs[i] & 15UL) != 0) {
            return fail("stress-realign", i);
        }
        fill_pattern((unsigned char *)ptrs[i], sizes[i], i + 101);
    }
    for (int i = 0; i < STRESS_COUNT; ++i) {
        int seed = (i & 1) ? i + 101 : i + 1;
        int bad = check_pattern((const unsigned char *)ptrs[i], sizes[i], seed);
        if (bad) {
            return fail("stress-check-b", i);
        }
    }
    for (int i = 0; i < STRESS_COUNT; ++i) {
        free(ptrs[i]);
    }

    unsigned char *big = malloc(12000);
    if (!big) {
        return fail("stress-big", -1);
    }
    fill_pattern(big, 12000, 77);
    if (check_pattern(big, 12000, 77)) {
        return fail("stress-big-check", -1);
    }
    free(big);
    return 0;
}

static int run_calloc_realloc_tests(void)
{
    if (calloc((size_t)-1, 2) != 0) {
        return fail("calloc-overflow", -1);
    }

    unsigned char *zero = calloc(64, 32);
    if (!zero) {
        return fail("calloc", -1);
    }
    for (int i = 0; i < 2048; ++i) {
        if (zero[i] != 0) {
            return fail("calloc-zero", i);
        }
    }
    fill_pattern(zero, 2048, 11);

    unsigned char *grown = realloc(zero, 4096);
    if (!grown) {
        free(zero);
        return fail("realloc-grow", -1);
    }
    if (check_pattern(grown, 2048, 11)) {
        free(grown);
        return fail("realloc-grow-check", -1);
    }
    fill_pattern(grown + 2048, 2048, 29);

    unsigned char *shrunk = realloc(grown, 128);
    if (!shrunk) {
        free(grown);
        return fail("realloc-shrink", -1);
    }
    if (check_pattern(shrunk, 128, 11)) {
        free(shrunk);
        return fail("realloc-shrink-check", -1);
    }
    free(shrunk);

    unsigned char *from_null = realloc(0, 96);
    if (!from_null) {
        return fail("realloc-null", -1);
    }
    fill_pattern(from_null, 96, 43);
    if (check_pattern(from_null, 96, 43)) {
        free(from_null);
        return fail("realloc-null-check", -1);
    }
    if (realloc(from_null, 0) != 0) {
        return fail("realloc-zero", -1);
    }
    return 0;
}

static int run_partial_munmap_tests(void)
{
    unsigned char *split = mmap(0, 12288, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (split == MAP_FAILED) {
        return fail("mmap-split", -1);
    }
    fill_pattern(split, 4096, 3);
    fill_pattern(split + 4096, 4096, 7);
    fill_pattern(split + 8192, 4096, 13);

    if (munmap(split + 4096, 4096) < 0) {
        return fail("munmap-middle", -1);
    }
    if (check_pattern(split, 4096, 3)) {
        return fail("munmap-left-check", -1);
    }
    if (check_pattern(split + 8192, 4096, 13)) {
        return fail("munmap-right-check", -1);
    }
    long middle_fault = write(1, split + 4096, 1);
    if (middle_fault != -14) {
        return fail("munmap-middle-efault", (int)middle_fault);
    }
    if (munmap(split, 4096) < 0) {
        return fail("munmap-left", -1);
    }
    if (munmap(split + 8192, 4096) < 0) {
        return fail("munmap-right", -1);
    }

    unsigned char *first = mmap(0, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (first == MAP_FAILED) {
        return fail("mmap-merge-first", -1);
    }
    unsigned char *second = mmap(0, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (second == MAP_FAILED) {
        munmap(first, 4096);
        return fail("mmap-merge-second", -1);
    }
    if (second != first + 4096) {
        munmap(first, 4096);
        munmap(second, 4096);
        return fail("mmap-merge-adjacent", -1);
    }
    first[0] = 0xa5;
    second[0] = 0x5a;
    if (munmap(first, 8192) < 0) {
        return fail("munmap-merged", -1);
    }
    long left_fault = write(1, first, 1);
    long right_fault = write(1, second, 1);
    if (left_fault != -14 || right_fault != -14) {
        return fail("munmap-merged-efault", (int)(left_fault + right_fault));
    }
    return 0;
}

static int has_prefix(const char *text, const char *prefix)
{
    for (int i = 0; prefix[i]; ++i) {
        if (text[i] != prefix[i]) {
            return 0;
        }
    }
    return 1;
}

static int has_substring(const char *text, const char *needle)
{
    for (int i = 0; text[i]; ++i) {
        if (has_prefix(text + i, needle)) {
            return 1;
        }
    }
    return 0;
}

static int run_file_mmap_tests(void)
{
    int fd = open(RELIEFOS_PATH_RELIEFOS_CONF, RELIEFOS_O_RDONLY, 0);
    if (fd < 0) {
        return fail("file-open", fd);
    }
    struct reliefos_stat st;
    if (reliefos_fstat_legacy(fd, &st) < 0) {
        close(fd);
        return fail("file-fstat", -1);
    }
    if (st.type != RELIEFOS_FS_TYPE_FILE || st.size >= 4096) {
        close(fd);
        return fail("file-stat", (int)st.size);
    }
    char *writable = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (writable != MAP_FAILED) {
        munmap(writable, 4096);
        close(fd);
        return fail("file-mmap-writable", -1);
    }
    char *bad_offset = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 1);
    if (bad_offset != MAP_FAILED) {
        munmap(bad_offset, 4096);
        close(fd);
        return fail("file-mmap-offset", -1);
    }

    char *map = mmap(0, 8192, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return fail("file-mmap", -1);
    }
    if (!has_substring(map, "CONFIG_IMAGE_SIZE_MIB=")) {
        munmap(map, 8192);
        close(fd);
        return fail("file-mmap-prefix", -1);
    }
    for (uint64_t i = st.size; i < 8192; ++i) {
        if (map[i] != 0) {
            munmap(map, 8192);
            close(fd);
            return fail("file-mmap-zero", (int)i);
        }
    }
    if (munmap(map, 8192) < 0) {
        close(fd);
        return fail("file-munmap", -1);
    }
    long fault = write(1, map, 1);
    close(fd);
    if (fault != -14) {
        return fail("file-munmap-efault", (int)fault);
    }
    return 0;
}

int main(void)
{
    puts("[memtest.elf] user memory test starting");

    unsigned char *map = mmap(0, 8192, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        return fail("mmap", -1);
    }
    for (int i = 0; i < 8192; ++i) {
        map[i] = (unsigned char)(i ^ 0x5a);
    }
    for (int i = 0; i < 8192; ++i) {
        if (map[i] != (unsigned char)(i ^ 0x5a)) {
            return fail("mmap-check", i);
        }
    }
    if (munmap(map, 8192) < 0) {
        return fail("munmap", -1);
    }
    long fault = write(1, map, 1);
    if (fault != -14) {
        return fail("unmapped-write-efault", (int)fault);
    }
    int partial = run_partial_munmap_tests();
    if (partial) {
        return partial;
    }
    int filemap = run_file_mmap_tests();
    if (filemap) {
        return filemap;
    }

    char *text = malloc(6000);
    if (!text) {
        return fail("malloc-large", -1);
    }
    for (int i = 0; i < 5999; ++i) {
        text[i] = (char)('A' + (i % 26));
    }
    text[5999] = 0;
    for (int i = 0; i < 5999; ++i) {
        if (text[i] != (char)('A' + (i % 26))) {
            return fail("malloc-large-check", i);
        }
    }
    free(text);

    char *small = malloc(32);
    if (!small) {
        return fail("malloc-small", -1);
    }
    small[0] = 'O';
    small[1] = 'K';
    small[2] = 0;
    printf("[memtest.elf] malloc string=%s\n", small);
    free(small);

    int stress = run_heap_stress();
    if (stress) {
        return stress;
    }
    int reallocs = run_calloc_realloc_tests();
    if (reallocs) {
        return reallocs;
    }

    puts("[memtest.elf] all checks passed");
    return 0;
}
