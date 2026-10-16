#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <setjmp.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VDSO_PAGE_SIZE 4096u
#define VDSO_CLOCKS 12u
#define VDSO_MODE_NONE 0u
#define VDSO_MODE_TSC 1u
#define VDSO_MAX_PHDRS 64u
#define VDSO_MAX_ROUNDS 9u
#define VDSO_BATCH_CALLS 128u
#define VDSO_MIGRATION_SAMPLES 32u
#define VDSO_ADJ_TICK_MIN_SAMPLES 16u
#define VDSO_ADJ_TICK_MAX_SAMPLES 128u
#define VDSO_ADJ_TICK_NS 9000L
#define VDSO_ADJ_TICK_TARGET_NS 50000000LL
#define VDSO_HRES_PROOF_MAX_ATTEMPTS 256u
#define VDSO_HRES_PROOF_MAX_WAIT_NS 100000000LL

struct vdso_timestamp {
    int64_t sec;
    uint64_t nsec;
};

/* This is the private, versioned kernel/vDSO protocol in the approved plan. */
struct vdso_data {
    uint32_t seq;
    uint32_t clock_mode;
    uint32_t clock_mask;
    uint32_t hres_mask;
    uint64_t cycle_last;
    uint64_t mult;
    uint64_t max_cycles;
    uint64_t resolution_ns;
    struct vdso_timestamp basetime[VDSO_CLOCKS];
};

typedef int (*vdso_clock_fn)(clockid_t, struct timespec *);

struct load_range {
    uintptr_t first;
    uintptr_t last;
    uint32_t flags;
};

struct vdso_image {
    uintptr_t ehdr;
    uintptr_t base;
    struct load_range loads[VDSO_MAX_PHDRS];
    size_t load_count;
    size_t symbol_count;
    const char *strtab;
    size_t strsz;
    const Elf64_Sym *symtab;
    const uint16_t *versym;
    const Elf64_Verdef *verdef;
    size_t verdef_count;
    vdso_clock_fn clock_gettime;
    vdso_clock_fn clock_getres;
    vdso_clock_fn clock_gettime_alias;
    vdso_clock_fn clock_getres_alias;
    bool has_dynamic;
    bool has_hash;
    bool has_soname;
    bool has_needed;
    bool has_interpreter;
    bool has_wx_load;
    bool has_relocations;
    bool has_version;
};

struct vdso_snapshot {
    uint32_t seq;
    uint32_t clock_mode;
    uint32_t clock_mask;
    uint32_t hres_mask;
    uint64_t cycle_last;
    uint64_t mult;
    uint64_t max_cycles;
    uint64_t resolution_ns;
    struct vdso_timestamp basetime[VDSO_CLOCKS];
};

struct clock_case {
    clockid_t id;
    const char *name;
};

static const struct clock_case clocks[] = {
    {CLOCK_REALTIME, "realtime"},
    {CLOCK_MONOTONIC, "monotonic"},
    {CLOCK_MONOTONIC_RAW, "monotonic_raw"},
    {CLOCK_REALTIME_COARSE, "realtime_coarse"},
    {CLOCK_MONOTONIC_COARSE, "monotonic_coarse"},
    {CLOCK_BOOTTIME, "boottime"},
    {CLOCK_TAI, "tai"},
};

static int failures;
static sigjmp_buf fault_env;
static volatile sig_atomic_t fault_expected;
static volatile uint64_t bench_sink;

static void fail(const char *what)
{
    ++failures;
    printf("[vdso] FAIL %s\n", what);
}

static void require(bool condition, const char *what)
{
    if (!condition)
        fail(what);
}

static bool pointer_in_load(const struct vdso_image *image, uintptr_t pointer,
                            size_t size, uint32_t required_flags)
{
    uintptr_t end;
    if (size > UINTPTR_MAX - pointer)
        return false;
    end = pointer + size;
    for (size_t i = 0; i < image->load_count; ++i) {
        const struct load_range *range = &image->loads[i];
        if (pointer >= range->first && end <= range->last &&
            (range->flags & required_flags) == required_flags)
            return true;
    }
    return false;
}

static uintptr_t dynamic_pointer(uintptr_t base, Elf64_Xword value)
{
    if (value > UINTPTR_MAX - base)
        return 0;
    return base + (uintptr_t)value;
}

static bool string_at(const struct vdso_image *image, size_t offset,
                      const char **value)
{
    if (!image->strtab || offset >= image->strsz)
        return false;
    const char *start = image->strtab + offset;
    if (!memchr(start, '\0', image->strsz - offset))
        return false;
    *value = start;
    return true;
}

static bool find_version(const struct vdso_image *image, const char *name,
                         uint16_t *index)
{
    uintptr_t cursor = (uintptr_t)image->verdef;
    for (size_t i = 0; i < image->verdef_count; ++i) {
        if (!pointer_in_load(image, cursor, sizeof(Elf64_Verdef), PF_R))
            return false;
        const Elf64_Verdef *definition = (const Elf64_Verdef *)cursor;
        uintptr_t auxiliary = cursor + definition->vd_aux;
        if (auxiliary < cursor ||
            !pointer_in_load(image, auxiliary, sizeof(Elf64_Verdaux), PF_R))
            return false;
        const Elf64_Verdaux *aux = (const Elf64_Verdaux *)auxiliary;
        const char *version_name;
        if (string_at(image, aux->vda_name, &version_name) &&
            strcmp(version_name, name) == 0 &&
            !(definition->vd_flags & VER_FLG_BASE)) {
            *index = definition->vd_ndx & 0x7fff;
            return true;
        }
        if (!definition->vd_next)
            break;
        if (definition->vd_next > UINTPTR_MAX - cursor)
            return false;
        cursor += definition->vd_next;
    }
    return false;
}

static bool version_matches(const struct vdso_image *image, uint16_t wanted,
                            const char *name)
{
    uint16_t index = 0;
    return find_version(image, name, &index) && index == wanted;
}

static uintptr_t lookup_symbol(const struct vdso_image *image,
                               const char *name, uint16_t wanted_version)
{
    for (size_t i = 0; i < image->symbol_count; ++i) {
        const Elf64_Sym *symbol = &image->symtab[i];
        const char *symbol_name;
        if (symbol->st_shndx == SHN_UNDEF ||
            ELF64_ST_TYPE(symbol->st_info) != STT_FUNC ||
            ELF64_ST_BIND(symbol->st_info) == STB_LOCAL ||
            !string_at(image, symbol->st_name, &symbol_name) ||
            strcmp(symbol_name, name) != 0)
            continue;
        if (!image->versym ||
            (image->versym[i] & 0x8000) ||
            (image->versym[i] & 0x7fff) != wanted_version ||
            !version_matches(image, wanted_version, "LINUX_2.6"))
            continue;
        uintptr_t address = dynamic_pointer(image->base, symbol->st_value);
        if (address && pointer_in_load(image, address, 1, PF_X))
            return address;
    }
    return 0;
}

static size_t gnu_symbol_count(const struct vdso_image *image, uintptr_t address)
{
    if (!pointer_in_load(image, address, 4 * sizeof(uint32_t), PF_R))
        return 0;
    const uint32_t *header = (const uint32_t *)address;
    uint32_t bucket_count = header[0];
    uint32_t symbol_offset = header[1];
    uint32_t bloom_count = header[2];
    if (!bucket_count || !bloom_count || bucket_count > 65536 ||
        bloom_count > 65536)
        return 0;
    uintptr_t buckets_addr = address + 4 * sizeof(uint32_t) +
                             (uintptr_t)bloom_count * sizeof(Elf64_Xword);
    size_t bucket_bytes = (size_t)bucket_count * sizeof(uint32_t);
    if (buckets_addr < address ||
        !pointer_in_load(image, buckets_addr, bucket_bytes, PF_R))
        return 0;
    const uint32_t *buckets = (const uint32_t *)buckets_addr;
    uint32_t max_symbol = 0;
    for (uint32_t i = 0; i < bucket_count; ++i)
        if (buckets[i] > max_symbol)
            max_symbol = buckets[i];
    if (max_symbol < symbol_offset)
        return symbol_offset;
    uintptr_t chains_addr = buckets_addr + bucket_bytes;
    uint32_t symbol = max_symbol;
    for (size_t count = 0; count < 65536; ++count, ++symbol) {
        uintptr_t chain_addr = chains_addr +
                              (uintptr_t)(symbol - symbol_offset) * sizeof(uint32_t);
        if (!pointer_in_load(image, chain_addr, sizeof(uint32_t), PF_R))
            return 0;
        uint32_t chain = *(const uint32_t *)chain_addr;
        if (chain & 1)
            return (size_t)symbol + 1;
    }
    return 0;
}

static bool inspect_vdso(uintptr_t ehdr_address, struct vdso_image *image)
{
    memset(image, 0, sizeof(*image));
    image->ehdr = ehdr_address;
    if (!ehdr_address)
        return false;
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)ehdr_address;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_type != ET_DYN || ehdr->e_machine != EM_X86_64 ||
        ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        !ehdr->e_phnum || ehdr->e_phnum > VDSO_MAX_PHDRS ||
        ehdr->e_phoff > UINTPTR_MAX - ehdr_address)
        return false;
    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(ehdr_address + ehdr->e_phoff);
    uintptr_t base = 0;
    for (size_t i = 0; i < ehdr->e_phnum; ++i) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type == PT_INTERP)
            image->has_interpreter = true;
        if (ph->p_type == PT_DYNAMIC)
            image->has_dynamic = true;
        if (ph->p_type != PT_LOAD)
            continue;
        if (image->load_count == VDSO_MAX_PHDRS)
            return false;
        uintptr_t candidate;
        if (ph->p_offset >= ph->p_vaddr) {
            if (ph->p_offset - ph->p_vaddr > UINTPTR_MAX - ehdr_address)
                return false;
            candidate = ehdr_address + (uintptr_t)(ph->p_offset - ph->p_vaddr);
        } else {
            if (ph->p_vaddr - ph->p_offset > ehdr_address)
                return false;
            candidate = ehdr_address - (uintptr_t)(ph->p_vaddr - ph->p_offset);
        }
        if (base && base != candidate)
            return false;
        base = candidate;
        uintptr_t first = candidate + (uintptr_t)ph->p_vaddr;
        if (first < candidate || ph->p_memsz > UINTPTR_MAX - first)
            return false;
        image->loads[image->load_count++] = (struct load_range){
            .first = first,
            .last = first + (uintptr_t)ph->p_memsz,
            .flags = ph->p_flags,
        };
        if ((ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
            image->has_wx_load = true;
    }
    image->base = base;
    if (!image->load_count || !image->has_dynamic || !base ||
        !pointer_in_load(image, ehdr_address, sizeof(*ehdr), PF_R) ||
        !pointer_in_load(image, (uintptr_t)phdrs,
                         (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr), PF_R))
        return false;

    const Elf64_Phdr *dynamic_phdr = NULL;
    for (size_t i = 0; i < ehdr->e_phnum; ++i)
        if (phdrs[i].p_type == PT_DYNAMIC)
            dynamic_phdr = &phdrs[i];
    if (!dynamic_phdr || dynamic_phdr->p_memsz < sizeof(Elf64_Dyn))
        return false;
    uintptr_t dynamic_address = dynamic_pointer(base, dynamic_phdr->p_vaddr);
    size_t dynamic_count = (size_t)(dynamic_phdr->p_memsz / sizeof(Elf64_Dyn));
    if (!dynamic_address ||
        !pointer_in_load(image, dynamic_address,
                         dynamic_count * sizeof(Elf64_Dyn), PF_R))
        return false;

    uintptr_t hash_address = 0, gnu_hash_address = 0, symtab_address = 0;
    uintptr_t versym_address = 0, verdef_address = 0;
    uintptr_t soname_offset = UINTPTR_MAX;
    size_t verdef_count = 0;
    size_t rela_size = 0, rel_size = 0, plt_rel_size = 0, relr_size = 0;
    const Elf64_Dyn *dynamic = (const Elf64_Dyn *)dynamic_address;
    bool terminated = false;
    for (size_t i = 0; i < dynamic_count; ++i) {
        Elf64_Xword value = dynamic[i].d_un.d_val;
        switch (dynamic[i].d_tag) {
        case DT_NULL:
            terminated = true;
            i = dynamic_count;
            break;
        case DT_STRTAB: image->strtab = (const char *)dynamic_pointer(base, value); break;
        case DT_STRSZ: image->strsz = (size_t)value; break;
        case DT_SYMTAB: symtab_address = dynamic_pointer(base, value); break;
        case DT_SYMENT:
            if (value != sizeof(Elf64_Sym)) return false;
            break;
        case DT_HASH: hash_address = dynamic_pointer(base, value); break;
        case DT_GNU_HASH: gnu_hash_address = dynamic_pointer(base, value); break;
        case DT_VERSYM: versym_address = dynamic_pointer(base, value); break;
        case DT_VERDEF: verdef_address = dynamic_pointer(base, value); break;
        case DT_VERDEFNUM: verdef_count = (size_t)value; break;
        case DT_SONAME: soname_offset = (uintptr_t)value; break;
        case DT_NEEDED: image->has_needed = true; break;
        case DT_RELSZ: rel_size = (size_t)value; break;
        case DT_RELASZ: rela_size = (size_t)value; break;
        case DT_PLTRELSZ: plt_rel_size = (size_t)value; break;
#ifdef DT_RELRSZ
        case DT_RELRSZ: relr_size = (size_t)value; break;
#endif
        case DT_TEXTREL: image->has_relocations = true; break;
        default: break;
        }
    }
    if (!terminated || !image->strtab || !image->strsz || !symtab_address ||
        !pointer_in_load(image, (uintptr_t)image->strtab, image->strsz, PF_R) ||
        !pointer_in_load(image, symtab_address, sizeof(Elf64_Sym), PF_R))
        return false;
    if (soname_offset != UINTPTR_MAX) {
        const char *soname;
        image->has_soname = string_at(image, soname_offset, &soname) &&
                            strcmp(soname, "linux-vdso.so.1") == 0;
    }
    if (hash_address) {
        if (!pointer_in_load(image, hash_address, 2 * sizeof(uint32_t), PF_R))
            return false;
        const uint32_t *hash = (const uint32_t *)hash_address;
        size_t nbucket = hash[0], nchain = hash[1];
        if (!nbucket || !nchain || nbucket > 65536 || nchain > 65536 ||
            !pointer_in_load(image, hash_address,
                             2 * sizeof(uint32_t) +
                             (nbucket + nchain) * sizeof(uint32_t), PF_R))
            return false;
        image->symbol_count = nchain;
        image->has_hash = true;
    } else if (gnu_hash_address) {
        image->symbol_count = gnu_symbol_count(image, gnu_hash_address);
        image->has_hash = image->symbol_count != 0;
    }
    if (!image->symbol_count ||
        image->symbol_count > SIZE_MAX / sizeof(Elf64_Sym) ||
        !pointer_in_load(image, symtab_address,
                         image->symbol_count * sizeof(Elf64_Sym), PF_R))
        return false;
    image->symtab = (const Elf64_Sym *)symtab_address;
    image->verdef = (const Elf64_Verdef *)verdef_address;
    image->verdef_count = verdef_count;
    if (!versym_address || !verdef_address || !verdef_count ||
        !pointer_in_load(image, versym_address,
                         image->symbol_count * sizeof(uint16_t), PF_R) ||
        !pointer_in_load(image, verdef_address, sizeof(Elf64_Verdef), PF_R))
        return false;
    image->versym = (const uint16_t *)versym_address;
    uint16_t linux_version = 0;
    image->has_version = find_version(image, "LINUX_2.6", &linux_version);
    if (!image->has_version)
        return false;
    image->has_relocations = image->has_relocations || rel_size || rela_size ||
                             plt_rel_size || relr_size;
    image->clock_gettime = (vdso_clock_fn)lookup_symbol(
        image, "__vdso_clock_gettime", linux_version);
    image->clock_getres = (vdso_clock_fn)lookup_symbol(
        image, "__vdso_clock_getres", linux_version);
    image->clock_gettime_alias = (vdso_clock_fn)lookup_symbol(
        image, "clock_gettime", linux_version);
    image->clock_getres_alias = (vdso_clock_fn)lookup_symbol(
        image, "clock_getres", linux_version);
    return true;
}

static bool valid_timespec(const struct timespec *value)
{
    return value->tv_nsec >= 0 && value->tv_nsec < 1000000000L;
}

static __int128 timespec_ns(const struct timespec *value)
{
    return (__int128)value->tv_sec * 1000000000 + value->tv_nsec;
}

static int raw_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "0"((long)SYS_clock_gettime), "D"((long)clock_id),
                       "S"((long)value)
                     : "rcx", "r11", "memory");
    return (int)result;
}

static int raw_clock_getres(clockid_t clock_id, struct timespec *value)
{
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "0"((long)SYS_clock_getres), "D"((long)clock_id),
                       "S"((long)value)
                     : "rcx", "r11", "memory");
    return (int)result;
}

static bool snapshot_vvar(const volatile struct vdso_data *data,
                          struct vdso_snapshot *snapshot)
{
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        uint32_t first = __atomic_load_n(&data->seq, __ATOMIC_ACQUIRE);
        if (first & 1u)
            continue;
        snapshot->clock_mode = data->clock_mode;
        snapshot->clock_mask = data->clock_mask;
        snapshot->hres_mask = data->hres_mask;
        snapshot->cycle_last = data->cycle_last;
        snapshot->mult = data->mult;
        snapshot->max_cycles = data->max_cycles;
        snapshot->resolution_ns = data->resolution_ns;
        for (size_t i = 0; i < VDSO_CLOCKS; ++i) {
            snapshot->basetime[i].sec = data->basetime[i].sec;
            snapshot->basetime[i].nsec = data->basetime[i].nsec;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t second = __atomic_load_n(&data->seq, __ATOMIC_RELAXED);
        if (first == second && !(second & 1u)) {
            snapshot->seq = second;
            return true;
        }
    }
    return false;
}

static void segv_handler(int signo)
{
    (void)signo;
    if (fault_expected)
        siglongjmp(fault_env, 1);
    _exit(128 + SIGSEGV);
}

static bool write_faults(volatile unsigned char *address)
{
    struct sigaction action = {0}, previous = {0};
    action.sa_handler = segv_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &previous) != 0)
        return false;
    volatile unsigned char saved = *address;
    int jumped = sigsetjmp(fault_env, 1);
    bool trapped = jumped != 0;
    if (!jumped) {
        fault_expected = 1;
        *address = saved;
    }
    fault_expected = 0;
    (void)sigaction(SIGSEGV, &previous, NULL);
    return trapped;
}

static bool check_vvar_permissions(uintptr_t ehdr)
{
    if (ehdr < VDSO_PAGE_SIZE)
        return false;
    volatile unsigned char *vvar = (volatile unsigned char *)(ehdr - VDSO_PAGE_SIZE);
    struct vdso_snapshot snapshot;
    if (!snapshot_vvar((const volatile struct vdso_data *)vvar, &snapshot))
        return false;
    if (mprotect((void *)vvar, VDSO_PAGE_SIZE, PROT_READ) != 0)
        return false;

    int writable = mprotect((void *)vvar, VDSO_PAGE_SIZE, PROT_READ | PROT_WRITE);
    int write_errno = errno;
    int executable = mprotect((void *)vvar, VDSO_PAGE_SIZE, PROT_READ | PROT_EXEC);
    int exec_errno = errno;
    int exec_only = mprotect((void *)vvar, VDSO_PAGE_SIZE, PROT_EXEC);
    int exec_only_errno = errno;
    require(writable == -1 && (write_errno == EACCES || write_errno == EPERM),
            "vvar rejects mprotect(PROT_READ|PROT_WRITE)");
    require(executable == -1 && (exec_errno == EACCES || exec_errno == EPERM),
            "vvar rejects mprotect(PROT_READ|PROT_EXEC)");
    require(exec_only == -1 && (exec_only_errno == EACCES || exec_only_errno == EPERM),
            "vvar rejects mprotect(PROT_EXEC)");
    require(write_faults(vvar + VDSO_PAGE_SIZE - 1), "vvar write faults while read-only");
    return mprotect((void *)vvar, VDSO_PAGE_SIZE, PROT_READ) == 0;
}

static bool check_code_permissions(uintptr_t ehdr)
{
    int result = mprotect((void *)ehdr, VDSO_PAGE_SIZE,
                          PROT_READ | PROT_WRITE);
    int saved_errno = errno;
    require(result == -1 && (saved_errno == EACCES || saved_errno == EPERM),
            "vDSO code rejects mprotect(PROT_READ|PROT_WRITE)");
    return result == -1;
}

static bool coarse_matches_snapshot(const struct vdso_image *image,
                                    clockid_t clock_id)
{
    const volatile struct vdso_data *data =
        (const volatile struct vdso_data *)(image->ehdr - VDSO_PAGE_SIZE);
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        uint32_t before = __atomic_load_n(&data->seq, __ATOMIC_ACQUIRE);
        if (before & 1u)
            continue;
        int64_t sec = data->basetime[clock_id].sec;
        uint64_t nsec = data->basetime[clock_id].nsec;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t middle = __atomic_load_n(&data->seq, __ATOMIC_RELAXED);
        if (before != middle || (middle & 1u))
            continue;
        struct timespec observed = {0};
        if (image->clock_gettime(clock_id, &observed) != 0)
            return false;
        uint32_t after = __atomic_load_n(&data->seq, __ATOMIC_ACQUIRE);
        if (after != middle || (after & 1u))
            continue;
        return observed.tv_sec == sec && (uint64_t)observed.tv_nsec == nsec;
    }
    return false;
}

static bool check_bracket(clockid_t id, const struct timespec *before,
                          const struct timespec *value,
                          const struct timespec *after,
                          const struct timespec *resolution)
{
    __int128 lo = timespec_ns(before);
    __int128 result = timespec_ns(value);
    __int128 hi = timespec_ns(after);
    __int128 tolerance = timespec_ns(resolution) * 2 + 1000000;
    if (id == CLOCK_REALTIME_COARSE || id == CLOCK_MONOTONIC_COARSE)
        tolerance += 10000000;
    if (tolerance < 1000000)
        tolerance = 1000000;
    return result >= lo - tolerance && result <= hi + tolerance;
}

static int read_direct_bracket(vdso_clock_fn function, clockid_t id,
                               struct timespec *result,
                               struct timespec *before,
                               struct timespec *after)
{
    int first = raw_clock_gettime(id, before);
    int call = function(id, result);
    int last = raw_clock_gettime(id, after);
    if (first != 0 || call != 0 || last != 0)
        return call ? call : first ? first : last;
    return 0;
}

static int read_libc_bracket(clockid_t id, struct timespec *result,
                             struct timespec *before,
                             struct timespec *after)
{
    int first = raw_clock_gettime(id, before);
    int call = clock_gettime(id, result);
    int call_errno = errno;
    int last = raw_clock_gettime(id, after);
    if (first != 0 || call != 0 || last != 0)
        return call ? -call_errno : first ? first : last;
    return 0;
}

static uint64_t read_tsc(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc; lfence"
                     : "=a"(low), "=d"(high)
                     :
                     : "memory");
    return ((uint64_t)high << 32) | low;
}

struct hres_window_proof {
    uint32_t seq;
    uint64_t tsc_before;
    uint64_t tsc_after;
    uint64_t cycle_last;
    uint64_t max_cycles;
};

static bool hres_clock_advertised(const struct vdso_snapshot *snapshot,
                                  clockid_t id)
{
    uint32_t bit = (uint32_t)1u << (unsigned int)id;
    return snapshot->clock_mode == VDSO_MODE_TSC &&
        (snapshot->clock_mask & bit) && (snapshot->hres_mask & bit);
}

static bool hres_interpolation_ns(const struct vdso_snapshot *snapshot,
                                  clockid_t id, uint64_t tsc,
                                  __int128 *result)
{
    if (id < 0 || (unsigned int)id >= VDSO_CLOCKS ||
        tsc < snapshot->cycle_last || !snapshot->mult ||
        !snapshot->max_cycles)
        return false;
    uint64_t cycles = tsc - snapshot->cycle_last;
    if (cycles > snapshot->max_cycles)
        return false;
    const struct vdso_timestamp *base = &snapshot->basetime[id];
    if (base->nsec >= 1000000000ULL)
        return false;
    __uint128_t scaled = (__uint128_t)cycles * snapshot->mult;
    scaled >>= 32;
    if (scaled > UINT64_MAX)
        return false;
    *result = (__int128)base->sec * 1000000000 + base->nsec + scaled;
    return true;
}

static void hres_retry_pause(void)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

/*
 * Verify one real direct-vDSO or libc call while the shared high-resolution
 * anchor remains stable. The surrounding serialized TSC reads bracket the
 * call; its output must agree with the same sequence's Q32 interpolation at
 * both bracket endpoints. Invalid or changing windows retry across PIT ticks,
 * but the search is bounded by 100 ms of raw CLOCK_MONOTONIC_COARSE time.
 */
static bool prove_hres_call_window(const struct vdso_image *image,
                                   clockid_t id, bool use_libc,
                                   struct hres_window_proof *proof)
{
    const volatile struct vdso_data *data =
        (const volatile struct vdso_data *)(image->ehdr - VDSO_PAGE_SIZE);
    struct timespec started = {0}, now = {0};
    bool have_deadline_clock = raw_clock_gettime(CLOCK_MONOTONIC_COARSE,
                                                  &started) == 0;

    for (unsigned int attempt = 0;
         attempt < VDSO_HRES_PROOF_MAX_ATTEMPTS; ++attempt) {
        if (have_deadline_clock &&
            raw_clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == 0 &&
            timespec_ns(&now) - timespec_ns(&started) >=
                VDSO_HRES_PROOF_MAX_WAIT_NS)
            break;

        struct vdso_snapshot snapshot;
        if (!snapshot_vvar(data, &snapshot)) {
            hres_retry_pause();
            continue;
        }
        if (!hres_clock_advertised(&snapshot, id))
            return false;
        if (!snapshot.mult || !snapshot.max_cycles)
            return false;

        uint64_t before = read_tsc();
        struct timespec value = {0};
        int call_result;
        if (use_libc) {
            errno = 0;
            call_result = clock_gettime(id, &value);
        } else {
            call_result = image->clock_gettime(id, &value);
        }
        uint64_t after = read_tsc();
        uint32_t seq_after = __atomic_load_n(&data->seq, __ATOMIC_ACQUIRE);

        if (seq_after != snapshot.seq || (seq_after & 1u) ||
            before < snapshot.cycle_last || after < snapshot.cycle_last ||
            before - snapshot.cycle_last > snapshot.max_cycles ||
            after - snapshot.cycle_last > snapshot.max_cycles) {
            hres_retry_pause();
            continue;
        }

        __int128 lower_ns, upper_ns;
        if (call_result != 0 || !valid_timespec(&value) ||
            !hres_interpolation_ns(&snapshot, id, before, &lower_ns) ||
            !hres_interpolation_ns(&snapshot, id, after, &upper_ns))
            return false;

        __int128 value_ns = timespec_ns(&value);
        if (value_ns < lower_ns - 1 || value_ns > upper_ns + 1)
            return false;

        proof->seq = snapshot.seq;
        proof->tsc_before = before;
        proof->tsc_after = after;
        proof->cycle_last = snapshot.cycle_last;
        proof->max_cycles = snapshot.max_cycles;
        printf("[vdso] HRES_WINDOW id=%d call=%s seq=%u tsc_before=%llu "
               "tsc_after=%llu cycle_last=%llu max_cycles=%llu "
               "q32_result_match=1 attempt=%u\n",
               id, use_libc ? "libc" : "direct", proof->seq,
               (unsigned long long)proof->tsc_before,
               (unsigned long long)proof->tsc_after,
               (unsigned long long)proof->cycle_last,
               (unsigned long long)proof->max_cycles, attempt + 1);
        return true;
    }
    return false;
}

static uint64_t run_clock_batch(clockid_t id, unsigned int count,
                               unsigned int which, vdso_clock_fn direct)
{
    uint64_t begin = read_tsc();
    struct timespec value;
    for (unsigned int i = 0; i < count; ++i) {
        int result;
        if (which == 0)
            result = direct(id, &value);
        else if (which == 1)
            result = raw_clock_gettime(id, &value);
        else
            result = clock_gettime(id, &value) == 0 ? 0 : -errno;
        if (result == 0)
            bench_sink += (uint64_t)value.tv_nsec + (uint64_t)value.tv_sec;
        else
            bench_sink += (uint64_t)(-result);
    }
    uint64_t end = read_tsc();
    return (end - begin) / count;
}

static void sort_u64(uint64_t *values, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        uint64_t value = values[i];
        size_t j = i;
        while (j && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static void benchmark_clock(clockid_t id, vdso_clock_fn direct,
                            uint64_t result[3])
{
    uint64_t samples[3][VDSO_MAX_ROUNDS];
    for (unsigned int round = 0; round < VDSO_MAX_ROUNDS; ++round) {
        for (unsigned int step = 0; step < 3; ++step) {
            unsigned int which = (step + round) % 3;
            samples[which][round] = run_clock_batch(id, VDSO_BATCH_CALLS,
                                                     which, direct);
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        sort_u64(samples[i], VDSO_MAX_ROUNDS);
        result[i] = samples[i][VDSO_MAX_ROUNDS / 2];
    }
}

static bool check_clock_case(const struct clock_case *test,
                             const struct vdso_image *image,
                             const struct vdso_snapshot *snapshot)
{
    struct timespec direct_before = {0}, direct_after = {0}, direct = {0};
    struct timespec libc_before = {0}, libc_after = {0}, libc_value = {0};
    struct timespec direct_res = {0}, raw_res = {0};
    int direct_result = read_direct_bracket(image->clock_gettime, test->id,
                                             &direct, &direct_before,
                                             &direct_after);
    require(direct_result == 0, "direct vDSO clock_gettime succeeds");
    require(valid_timespec(&direct), "direct vDSO timespec is normalized");
    require(valid_timespec(&direct_before) && valid_timespec(&direct_after),
            "syscall bracket timespecs are normalized");
    int libc_result = read_libc_bracket(test->id, &libc_value, &libc_before,
                                        &libc_after);
    require(libc_result == 0, "musl clock_gettime succeeds");
    require(valid_timespec(&libc_value), "musl timespec is normalized");
    int getres_result = image->clock_getres(test->id, &direct_res);
    int raw_res_result = raw_clock_getres(test->id, &raw_res);
    require(getres_result == 0 && raw_res_result == 0,
            "direct and raw clock_getres succeed");
    require(valid_timespec(&direct_res) && valid_timespec(&raw_res),
            "clock_getres returns normalized resolution");
    if (getres_result == 0 && raw_res_result == 0)
        require(timespec_ns(&direct_res) == timespec_ns(&raw_res),
                "vDSO and syscall clock_getres agree");
    require(image->clock_getres(test->id, NULL) == 0,
            "direct clock_getres accepts NULL output for valid clock");
    require(raw_clock_getres(test->id, NULL) == 0,
            "raw clock_getres accepts NULL output for valid clock");
    require(clock_getres(test->id, NULL) == 0,
            "musl clock_getres accepts NULL output for valid clock");

    bool direct_bracket_ok = direct_result == 0 && getres_result == 0 &&
        check_bracket(test->id, &direct_before, &direct, &direct_after, &direct_res);
    require(direct_bracket_ok, "direct clock result lies within syscall bracket");
    bool libc_bracket_ok = libc_result == 0 && getres_result == 0 &&
        check_bracket(test->id, &libc_before, &libc_value, &libc_after, &direct_res);
    require(libc_bracket_ok, "musl clock result lies within syscall bracket");

    uint32_t bit = (uint32_t)1u << (unsigned int)test->id;
    const char *mode = "fallback";
    bool hres_advertised = false;
    bool direct_window_match = false;
    bool libc_window_match = false;
    bool hres_proof_ok = true;
    bool highres_clock = test->id == CLOCK_REALTIME ||
        test->id == CLOCK_MONOTONIC;
    if (highres_clock) {
        struct vdso_snapshot current;
        bool current_ok = snapshot_vvar(
            (const volatile struct vdso_data *)(image->ehdr - VDSO_PAGE_SIZE),
            &current);
        require(current_ok, "read stable vvar state before hres proof");
        if (current_ok && hres_clock_advertised(&current, test->id)) {
            struct hres_window_proof direct_proof = {0}, libc_proof = {0};
            hres_advertised = true;
            direct_window_match = prove_hres_call_window(
                image, test->id, false, &direct_proof);
            libc_window_match = prove_hres_call_window(
                image, test->id, true, &libc_proof);
            require(direct_window_match,
                    "direct vDSO call matches stable Q32 hres window");
            require(libc_window_match,
                    "libc call matches stable Q32 hres window");
            hres_proof_ok = direct_window_match && libc_window_match;
            mode = hres_proof_ok ? "hres-enabled-window-proven" :
                                   "hres-enabled-unproven";
        }
    }
    if (test->id == CLOCK_REALTIME_COARSE ||
               test->id == CLOCK_MONOTONIC_COARSE) {
        if ((snapshot->clock_mask & bit) && !(snapshot->hres_mask & bit))
            mode = "coarse-fast";
    }
    bool coarse_match = true;
    if (test->id == CLOCK_REALTIME_COARSE ||
        test->id == CLOCK_MONOTONIC_COARSE) {
        coarse_match = coarse_matches_snapshot(image, test->id);
        require(coarse_match,
                "coarse vDSO result matches a stable private vvar baseline");
        if (!coarse_match)
            mode = "fallback-or-mismatch";
    }

    uint64_t timings[3] = {0};
    benchmark_clock(test->id, image->clock_gettime, timings);
    printf("[vdso] CLOCK name=%s id=%d direct=ok libc=ok raw=ok mode=%s",
           test->name, test->id, mode);
    if (highres_clock) {
        printf(" hres_advertised=%d direct_q32_window=%d "
               "libc_q32_window=%d benchmark_path=%s",
               hres_advertised ? 1 : 0,
               direct_window_match ? 1 : 0,
               libc_window_match ? 1 : 0,
               hres_advertised ? "may-fallback" : "fallback");
    }
    if (test->id == CLOCK_REALTIME_COARSE ||
        test->id == CLOCK_MONOTONIC_COARSE)
        printf(" coarse_snapshot_match=%d", coarse_match ? 1 : 0);
    printf(" median_cycles_direct=%llu_libc=%llu_raw=%llu\n",
           (unsigned long long)timings[0],
           (unsigned long long)timings[2],
           (unsigned long long)timings[1]);
    return direct_bracket_ok && libc_bracket_ok && hres_proof_ok;
}

static void check_invalid_ids(const struct vdso_image *image)
{
    const clockid_t invalid[] = {-1, INT_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        struct timespec value;
        int direct = image->clock_gettime(invalid[i], &value);
        require(direct == -EINVAL, "vDSO invalid clock returns -EINVAL");
        errno = 0;
        int libc = clock_gettime(invalid[i], &value);
        int libc_errno = errno;
        require(libc == -1 && libc_errno == EINVAL,
                "musl invalid clock returns EINVAL");
        require(image->clock_getres(invalid[i], NULL) == -EINVAL,
                "vDSO invalid clock_getres NULL returns -EINVAL");
        require(raw_clock_getres(invalid[i], NULL) == -EINVAL,
                "raw invalid clock_getres NULL returns -EINVAL");
        errno = 0;
        libc = clock_getres(invalid[i], NULL);
        libc_errno = errno;
        require(libc == -1 && libc_errno == EINVAL,
                "musl invalid clock_getres NULL returns EINVAL");
    }
}

static bool sample_monotonic_triplet(vdso_clock_fn direct,
                                     struct timespec values[3],
                                     const char *test_name)
{
    int direct_result = direct(CLOCK_MONOTONIC, &values[0]);
    if (direct_result != 0) {
        printf("[vdso] FAIL %s direct monotonic read returned %d\n",
               test_name, direct_result);
        ++failures;
        return false;
    }
    int libc_result = clock_gettime(CLOCK_MONOTONIC, &values[1]);
    int libc_errno = errno;
    if (libc_result != 0) {
        printf("[vdso] FAIL %s libc monotonic read errno=%d\n",
               test_name, libc_errno);
        ++failures;
        return false;
    }
    int raw_result = raw_clock_gettime(CLOCK_MONOTONIC, &values[2]);
    if (raw_result != 0) {
        printf("[vdso] FAIL %s syscall monotonic read returned %d\n",
               test_name, raw_result);
        ++failures;
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!valid_timespec(&values[i])) {
            printf("[vdso] FAIL %s monotonic source=%zu returned unnormalized nsec=%ld\n",
                   test_name, i, values[i].tv_nsec);
            ++failures;
            return false;
        }
    }
    return true;
}

static bool monotonic_triplet_nonregression(vdso_clock_fn direct,
                                            struct timespec previous[3],
                                            bool have_previous[3],
                                            const char *test_name)
{
    struct timespec current[3];
    static const char *const source_names[] = {"direct", "libc", "syscall"};
    if (!sample_monotonic_triplet(direct, current, test_name))
        return false;
    for (size_t i = 0; i < 3; ++i) {
        if (have_previous[i] && timespec_ns(&current[i]) < timespec_ns(&previous[i])) {
            printf("[vdso] FAIL %s monotonic moved backwards source=%s\n",
                   test_name, source_names[i]);
            ++failures;
            return false;
        }
        previous[i] = current[i];
        have_previous[i] = true;
    }
    return true;
}

static bool check_cpu_migration(vdso_clock_fn direct)
{
    uint64_t original_mask = 0;
    long get_result = syscall(SYS_sched_getaffinity, 0,
                              (long)sizeof(original_mask), &original_mask);
    if (get_result != (long)sizeof(original_mask) || !original_mask) {
        printf("[vdso] FAIL sched_getaffinity returned %ld mask=%#llx errno=%d\n",
               get_result, (unsigned long long)original_mask, errno);
        ++failures;
        return false;
    }

    unsigned int cpu_count = (unsigned int)__builtin_popcountll(original_mask);
    printf("[vdso] CPU_MIGRATION allowed_mask=%#llx cpu_count=%u topology=%s\n",
           (unsigned long long)original_mask, cpu_count,
           cpu_count == 1 ? "oneCPU" : "multiCPU");

    struct timespec previous[3] = {{0}};
    bool have_previous[3] = {false, false, false};
    bool all_ok = true;
    for (unsigned int cpu = 0; cpu < 64; ++cpu) {
        uint64_t bit = UINT64_C(1) << cpu;
        if (!(original_mask & bit))
            continue;
        if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(bit), &bit) != 0) {
            printf("[vdso] FAIL sched_setaffinity cpu=%u errno=%d\n", cpu, errno);
            ++failures;
            all_ok = false;
            continue;
        }

        bool confirmed = false;
        uint32_t actual_cpu = UINT32_MAX;
        for (unsigned int poll = 0; poll < 256; ++poll) {
            if (syscall(SYS_sched_yield) != 0) {
                printf("[vdso] FAIL sched_yield cpu=%u errno=%d\n", cpu, errno);
                ++failures;
                break;
            }
            actual_cpu = UINT32_MAX;
            if (syscall(SYS_getcpu, &actual_cpu, NULL, NULL) != 0) {
                printf("[vdso] FAIL getcpu cpu=%u errno=%d\n", cpu, errno);
                ++failures;
                break;
            }
            if (actual_cpu == cpu) {
                confirmed = true;
                break;
            }
        }
        if (!confirmed) {
            printf("[vdso] FAIL affinity cpu=%u actual_cpu=%u was not confirmed\n",
                   cpu, actual_cpu);
            ++failures;
            all_ok = false;
            continue;
        }

        unsigned int samples = 0;
        for (; samples < VDSO_MIGRATION_SAMPLES; ++samples) {
            if (!monotonic_triplet_nonregression(direct, previous,
                                                 have_previous,
                                                 "cpu_migration")) {
                all_ok = false;
                break;
            }
        }
        printf("[vdso] CPU_MIGRATION cpu=%u confirmed=1 samples=%u\n",
               cpu, samples);
    }

    if (syscall(SYS_sched_setaffinity, 0, (long)sizeof(original_mask),
                &original_mask) != 0) {
        printf("[vdso] FAIL sched_setaffinity restore errno=%d\n", errno);
        ++failures;
        all_ok = false;
    } else {
        uint64_t restored_mask = 0;
        long verify_result = syscall(SYS_sched_getaffinity, 0,
                                     (long)sizeof(restored_mask),
                                     &restored_mask);
        if (verify_result != (long)sizeof(restored_mask) ||
            restored_mask != original_mask) {
            printf("[vdso] FAIL affinity restore expected=%#llx observed=%#llx\n",
                   (unsigned long long)original_mask,
                   (unsigned long long)restored_mask);
            ++failures;
            all_ok = false;
        } else {
            printf("[vdso] CPU_MIGRATION restored_mask=%#llx verified=1\n",
                   (unsigned long long)restored_mask);
        }
    }
    return all_ok;
}

static bool check_negative_adj_tick(vdso_clock_fn direct)
{
    _Static_assert(sizeof(struct timex) == 208, "native x86-64 timex ABI");
    struct timex original = {0};
    errno = 0;
    int query_state = adjtimex(&original);
    int query_errno = errno;
    bool root = geteuid() == 0;
    bool cap_sys_time = false;
    bool changed = false;
    bool restored = true;
    bool all_ok = true;
    long old_tick = original.tick;
    unsigned int samples = 0;
    __int128 coarse_elapsed_ns = 0;
    struct timespec previous[3] = {{0}};
    bool have_previous[3] = {false, false, false};

    if (query_state < 0) {
        printf("[vdso] FAIL adjtimex query errno=%d\n", query_errno);
        ++failures;
        all_ok = false;
        goto report;
    }
    if (!root) {
        printf("[vdso] FAIL time adjustment requires root euid=%u\n",
               (unsigned int)geteuid());
        ++failures;
        all_ok = false;
        goto report;
    }
    if (old_tick < 9000 || old_tick > 11000) {
        printf("[vdso] FAIL adjtimex reported out-of-range old tick=%ld\n",
               old_tick);
        ++failures;
        all_ok = false;
        goto report;
    }
    if (!monotonic_triplet_nonregression(direct, previous, have_previous,
                                         "adj_tick_before")) {
        all_ok = false;
        goto report;
    }

    struct timex adjustment = {0};
    adjustment.modes = ADJ_TICK;
    adjustment.tick = VDSO_ADJ_TICK_NS;
    errno = 0;
    int adjust_state = adjtimex(&adjustment);
    int adjust_errno = errno;
    cap_sys_time = adjust_state >= 0;
    if (!cap_sys_time) {
        printf("[vdso] FAIL ADJ_TICK=9000 rejected errno=%d\n", adjust_errno);
        ++failures;
        all_ok = false;
        goto report;
    }
    changed = true;

    struct timex verify_adjustment = {0};
    if (adjtimex(&verify_adjustment) < 0 ||
        verify_adjustment.tick != VDSO_ADJ_TICK_NS) {
        printf("[vdso] FAIL adjtimex did not retain requested tick=9000 observed=%ld errno=%d\n",
               verify_adjustment.tick, errno);
        ++failures;
        all_ok = false;
    }
    if (!monotonic_triplet_nonregression(direct, previous, have_previous,
                                         "adj_tick_transition"))
        all_ok = false;

    struct timespec coarse_start = {0}, coarse_now = {0};
    int coarse_result = raw_clock_gettime(CLOCK_MONOTONIC_COARSE, &coarse_start);
    if (coarse_result != 0 || !valid_timespec(&coarse_start)) {
        printf("[vdso] FAIL adj_tick coarse start read returned %d\n", coarse_result);
        ++failures;
        all_ok = false;
        goto restore;
    }

    while (samples < VDSO_ADJ_TICK_MAX_SAMPLES) {
        if (!monotonic_triplet_nonregression(direct, previous, have_previous,
                                             "adj_tick_negative")) {
            all_ok = false;
            break;
        }
        ++samples;

        coarse_result = raw_clock_gettime(CLOCK_MONOTONIC_COARSE, &coarse_now);
        if (coarse_result != 0 || !valid_timespec(&coarse_now)) {
            printf("[vdso] FAIL adj_tick coarse sample read returned %d\n",
                   coarse_result);
            ++failures;
            all_ok = false;
            break;
        }
        coarse_elapsed_ns = timespec_ns(&coarse_now) - timespec_ns(&coarse_start);
        if (samples >= VDSO_ADJ_TICK_MIN_SAMPLES &&
            coarse_elapsed_ns >= VDSO_ADJ_TICK_TARGET_NS)
            break;

        struct timespec delay = {.tv_sec = 0, .tv_nsec = 2000000};
        while (nanosleep(&delay, &delay) != 0) {
            if (errno == EINTR)
                continue;
            printf("[vdso] FAIL adj_tick nanosleep errno=%d\n", errno);
            ++failures;
            all_ok = false;
            goto restore;
        }
    }
    if (samples < VDSO_ADJ_TICK_MIN_SAMPLES || coarse_elapsed_ns < 30000000LL) {
        printf("[vdso] FAIL adj_tick did not span several PIT ticks samples=%u elapsed_ns=%lld\n",
               samples, (long long)coarse_elapsed_ns);
        ++failures;
        all_ok = false;
    }

restore:
    if (changed) {
        struct timex restore_tick = {0};
        restore_tick.modes = ADJ_TICK;
        restore_tick.tick = old_tick;
        errno = 0;
        int restore_state = adjtimex(&restore_tick);
        int restore_errno = errno;
        restored = restore_state >= 0;
        if (restored) {
            struct timex verify_restore = {0};
            restored = adjtimex(&verify_restore) >= 0 &&
                       verify_restore.tick == old_tick;
        }
        if (!restored) {
            printf("[vdso] FAIL adjtimex restore tick=%ld errno=%d\n",
                   old_tick, restore_errno);
            ++failures;
            all_ok = false;
        } else if (!monotonic_triplet_nonregression(direct, previous,
                                                    have_previous,
                                                    "adj_tick_restore")) {
            all_ok = false;
        }
    }

report:
    printf("[vdso] ADJ_TICK euid=%u cap_sys_time=%d old_tick=%ld requested_tick=%ld restored=%d samples=%u coarse_elapsed_ns=%lld\n",
           (unsigned int)geteuid(), cap_sys_time ? 1 : 0, old_tick,
           VDSO_ADJ_TICK_NS, restored ? 1 : 0, samples,
           (long long)coarse_elapsed_ns);
    return all_ok && cap_sys_time && restored;
}

static bool check_fork(uintptr_t ehdr)
{
    uintptr_t vvar = ehdr - VDSO_PAGE_SIZE;
    volatile unsigned char *sentinel =
        (volatile unsigned char *)(vvar + VDSO_PAGE_SIZE - 1);
    unsigned char before = *sentinel;
    require(before == 0, "unused vvar bytes are zero-filled");
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        struct vdso_image child_image;
        uintptr_t child_ehdr = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
        struct timespec now;
        bool good = child_ehdr == ehdr && inspect_vdso(child_ehdr, &child_image) &&
                    child_image.clock_gettime &&
                    child_image.clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                    valid_timespec(&now) && write_faults(sentinel);
        _exit(good ? 0 : 1);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return false;
    bool same_mapping = (uintptr_t)getauxval(AT_SYSINFO_EHDR) == ehdr;
    bool child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    bool unchanged = *sentinel == before;
    require(same_mapping, "fork inherits vDSO auxv address");
    require(child_ok, "fork child resolves and calls inherited vDSO");
    require(unchanged, "fork child cannot mutate shared vvar byte");
    return same_mapping && child_ok && unchanged;
}

static bool exec_child(uintptr_t expected_parent)
{
    uintptr_t ehdr = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
    struct vdso_image image;
    struct timespec now;
    bool good = ehdr && inspect_vdso(ehdr, &image) && image.clock_gettime &&
                image.clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                valid_timespec(&now);
    printf("[vdso] EXEC child_ehdr=%#lx parent_ehdr=%#lx reinitialized=%d\n",
           (unsigned long)ehdr, (unsigned long)expected_parent, good ? 1 : 0);
    return good;
}

static bool check_exec(const char *program, uintptr_t parent_ehdr)
{
    char parent[32];
    (void)snprintf(parent, sizeof(parent), "%#lx", (unsigned long)parent_ehdr);
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        execl(program, program, "--exec-child", parent, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return false;
    bool good = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    require(good, "exec creates a fresh process with a usable vDSO");
    return good;
}

static bool check_required_symbols(const struct vdso_image *image)
{
    require(image->has_dynamic, "vDSO has PT_DYNAMIC");
    require(image->has_hash, "vDSO has a usable dynamic hash table");
    require(image->has_soname, "vDSO SONAME is linux-vdso.so.1");
    require(!image->has_interpreter, "vDSO has no PT_INTERP");
    require(!image->has_needed, "vDSO has no DT_NEEDED");
    require(!image->has_wx_load, "vDSO has no writable executable PT_LOAD");
    require(!image->has_relocations, "vDSO has no runtime relocations");
    require(image->has_version, "vDSO defines LINUX_2.6");
    require(image->clock_gettime != NULL,
            "versioned __vdso_clock_gettime@LINUX_2.6 resolves");
    require(image->clock_getres != NULL,
            "versioned __vdso_clock_getres@LINUX_2.6 resolves");
    require(image->clock_gettime_alias != NULL,
            "versioned clock_gettime@LINUX_2.6 alias resolves");
    require(image->clock_getres_alias != NULL,
            "versioned clock_getres@LINUX_2.6 alias resolves");
    return image->clock_gettime && image->clock_getres &&
           image->clock_gettime_alias && image->clock_getres_alias;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--exec-child") == 0) {
        uintptr_t parent = (uintptr_t)strtoull(argv[2], NULL, 0);
        return exec_child(parent) ? 0 : 1;
    }
    bool require_hres = argc == 2 && strcmp(argv[1], "--require-hres") == 0;

    uintptr_t ehdr = (uintptr_t)getauxval(AT_SYSINFO_EHDR);
    if (!ehdr) {
        puts("[vdso] FAIL missing AT_SYSINFO_EHDR");
        puts("[vdso] DONE failures=1");
        return 1;
    }

    struct vdso_image image;
    if (!inspect_vdso(ehdr, &image)) {
        fail("AT_SYSINFO_EHDR does not point to a valid ELF64 vDSO");
        puts("[vdso] DONE failures=1");
        return 1;
    }
    if (!check_required_symbols(&image)) {
        puts("[vdso] DONE failures=1");
        return 1;
    }

    const volatile struct vdso_data *data =
        (const volatile struct vdso_data *)(ehdr - VDSO_PAGE_SIZE);
    struct vdso_snapshot snapshot;
    bool snapshot_ok = snapshot_vvar(data, &snapshot);
    require(snapshot_ok, "vvar publishes a stable clock snapshot");
    if (!snapshot_ok) {
        puts("[vdso] DONE failures=1");
        return 1;
    }
    if (require_hres) {
        unsigned int attempt = 0;
        /* Give boot calibration quiet timer intervals before the desktop
         * starts. A strict coverage run must fail rather than count fallback
         * as evidence of the high-resolution user-mode path. */
        while (!(hres_clock_advertised(&snapshot, CLOCK_MONOTONIC) &&
                 hres_clock_advertised(&snapshot, CLOCK_REALTIME)) &&
               attempt < 500) {
            hres_retry_pause();
            snapshot_ok = snapshot_vvar(data, &snapshot);
            if (!snapshot_ok) break;
            ++attempt;
        }
        bool ready = snapshot_ok &&
            hres_clock_advertised(&snapshot, CLOCK_MONOTONIC) &&
            hres_clock_advertised(&snapshot, CLOCK_REALTIME);
        printf("[vdso] CALIBRATION_WAIT attempts=%u ready=%d\n", attempt, ready);
        require(ready, "required high-resolution mode becomes available");
    }
    printf("[vdso] INFO ehdr=%#lx vvar=%#lx mode=%u mask=%#x hres_mask=%#x seq=%u resolution_ns=%llu\n",
           (unsigned long)ehdr, (unsigned long)(ehdr - VDSO_PAGE_SIZE),
           snapshot.clock_mode, snapshot.clock_mask, snapshot.hres_mask,
           snapshot.seq, (unsigned long long)snapshot.resolution_ns);
    require(snapshot.clock_mode == VDSO_MODE_NONE ||
            snapshot.clock_mode == VDSO_MODE_TSC,
            "vvar clock mode is a supported value");
    require(!(snapshot.hres_mask & ~snapshot.clock_mask),
            "high-resolution mask is a subset of the snapshot mask");
    require((snapshot.hres_mask & ~((1u << CLOCK_REALTIME) |
                                    (1u << CLOCK_MONOTONIC))) == 0,
            "only realtime and monotonic use the TSC fast path");
    require((snapshot.clock_mask & ((1u << CLOCK_REALTIME_COARSE) |
                                    (1u << CLOCK_MONOTONIC_COARSE))) ==
            ((1u << CLOCK_REALTIME_COARSE) |
             (1u << CLOCK_MONOTONIC_COARSE)),
            "both coarse clocks have shared snapshot data");
    require(snapshot.clock_mode == VDSO_MODE_TSC ||
            !(snapshot.hres_mask & ((1u << CLOCK_REALTIME) |
                                    (1u << CLOCK_MONOTONIC))),
            "untrusted high-resolution clocks do not advertise TSC fast paths");

    require(check_vvar_permissions(ehdr),
            "vvar is readable and remains read-only/NX");
    require(check_code_permissions(ehdr),
            "vDSO code remains executable and rejects writes");
    for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i)
        (void)check_clock_case(&clocks[i], &image, &snapshot);
    check_invalid_ids(&image);
    require(check_cpu_migration(image.clock_gettime),
            "clock methods remain monotonic after pinning to each allowed CPU");
    require(check_negative_adj_tick(image.clock_gettime),
            "negative ADJ_TICK preserves monotonicity and restores prior tick");
    (void)check_fork(ehdr);
    (void)check_exec(argv[0], ehdr);
    printf("[vdso] DONE failures=%d\n", failures);
    return failures ? 1 : 0;
}
