#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device.h"
#include "../../kernel/reliefnt/drivers/video/framebuffer/framebuffer.c"

#if defined(SVGA_BATCH_SUBMIT_FAILURE_SNAPSHOT)
#define assert_batch_submit_failure(batch, expected_reason, expected_index, \
                                    expected_detail0, expected_detail1, \
                                    expected_detail2) do { \
    assert((batch).submit_failure.reason == (expected_reason)); \
    assert((batch).submit_failure.index == (expected_index)); \
    assert((batch).submit_failure.detail0 == (expected_detail0)); \
    assert((batch).submit_failure.detail1 == (expected_detail1)); \
    assert((batch).submit_failure.detail2 == (expected_detail2)); \
} while (0)
#else
#define assert_batch_submit_failure(...) do { \
    assert(!"SVGA final-submit failure snapshot is missing"); \
} while (0)
#endif

#define SVGA_TEST_SURFACE_BIND_VERTEX_BUFFER (1u << 20)
#define SVGA_TEST_SURFACE_BIND_INDEX_BUFFER (1u << 21)
#define SVGA_TEST_SURFACE_BIND_CONSTANT_BUFFER (1u << 22)
#define SVGA_TEST_SURFACE_BIND_SHADER_RESOURCE (1u << 23)
#define SVGA_TEST_SURFACE_BIND_RENDER_TARGET (1u << 24)
#define SVGA_TEST_SURFACE_BIND_DEPTH_STENCIL (1u << 25)
#define SVGA_TEST_SURFACE_BIND_STREAM_OUTPUT (1u << 26)
_Static_assert(SVGA3D_SURFACE_BIND_VERTEX_BUFFER == SVGA_TEST_SURFACE_BIND_VERTEX_BUFFER, "SVGA vertex bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_INDEX_BUFFER == SVGA_TEST_SURFACE_BIND_INDEX_BUFFER, "SVGA index bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_CONSTANT_BUFFER == SVGA_TEST_SURFACE_BIND_CONSTANT_BUFFER, "SVGA constant bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_SHADER_RESOURCE == SVGA_TEST_SURFACE_BIND_SHADER_RESOURCE, "SVGA shader-resource bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_RENDER_TARGET == SVGA_TEST_SURFACE_BIND_RENDER_TARGET, "SVGA render-target bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_DEPTH_STENCIL == SVGA_TEST_SURFACE_BIND_DEPTH_STENCIL, "SVGA depth-stencil bind bit");
_Static_assert(SVGA3D_SURFACE_BIND_STREAM_OUTPUT == SVGA_TEST_SURFACE_BIND_STREAM_OUTPUT, "SVGA stream-output bind bit");

#define FIFO_BYTES 65536u
static uint32_t fifo[FIFO_BYTES / 4], regs[64];
static uint32_t wide_fifo[(512u * 1024u) / 4u];
static uint32_t wide_batch_storage[SVGA_BATCH_MAX_OUTPUT_BYTES / 4u];
static uint32_t wide_payload[SVGA_BATCH_MAX_OUTPUT_BYTES / 4u];
static uint32_t capabilities, host_version, fifo_caps, selected_gmr;
static uint32_t host_devcap_3d, devcap_queries;
static uint32_t host_context_cap = SVGA_CONTEXTS;
static uint32_t host_surface_cap = SVGA_SURFACES;
static uint32_t host_texture_width_cap = 4096;
static uint32_t host_texture_height_cap = 4096;
static uint32_t host_render_targets_cap = UINT32_MAX;
static uint32_t configured_guest, command_count, draw_count, present_count;
static uint32_t visible_updates, visible_presents;
static uint32_t sync_writes, busy_reads;
static uint32_t host_delay_reads;
static uint32_t triangle_uploads;
static uint32_t readback_count, transform_count, depth_draw_count;
static uint32_t stall_command;
static uint64_t device_ticks;
static uint32_t host_targets[SVGA_CONTEXTS][SVGA3D_RT_MAX];
static uint32_t host_depth_enabled[SVGA_CONTEXTS], host_fill[SVGA_CONTEXTS];
static uint32_t host_projection[SVGA_CONTEXTS][16];
struct host_backing { uint32_t root, depth, bytes; };
static struct host_backing host_mobs[SVGA_MOBS], host_otables[5], retired_backings[512];
static uint32_t retired_count, host_context_mobs[SVGA_CONTEXTS];
static bool host_contexts[SVGA_CONTEXTS];
struct host_shader { uint32_t type, bytes, mobid; bool defined; };
static struct host_shader host_shaders[SVGA_GB_SHADER_SLOTS];
static uint32_t host_shaders_bound[SVGA_CONTEXTS][2];
struct host_query { uint32_t mobid, offset; bool active, pending; };
static struct host_query host_queries[SVGA_CONTEXTS];
struct host_surface { uint32_t width, height, format, mobid; uint8_t *data; bool gb; };
static struct host_surface host_surfaces[SVGA_SURFACES];
static bool stalled, invalid_caps_record, host_busy_supported;
static bool expect_guest_fifo_busy;
struct allocation { uint64_t phys; void *ptr; uint32_t pages; };
static struct allocation allocations[512];
static uint64_t next_phys = 0x10000000;
static uint32_t live_pages, fail_after = UINT32_MAX, allocation_calls;
static uint32_t host_gmr_pages[SVGA_GMRS], host_gmr_ppn[SVGA_GMRS][SVGA_MAX_GUEST_PAGES];
static uint32_t selected_port_reg, port_busy_reads, port_sync_writes;
static uint32_t batch_storage[SVGA_BATCH_MAX_BYTES / sizeof(uint32_t)];
static uint32_t heap_alloc_calls, heap_fail_after = UINT32_MAX, heap_live_allocations;
static uint64_t heap_live_bytes, heap_live_pages;
static bool assert_heap_unlocked;
static uint64_t assert_unlocked_free_phys;
static uint32_t unlocked_free_checks;
static bool assert_console_log_unlocked;
static uint32_t console_log_count;
static char console_log_text[256];

union svga_test_heap_header {
    max_align_t alignment;
    struct { size_t requested_bytes; } allocation;
};

static uint64_t heap_pages_for_size(size_t size)
{
    return size / 4096u + (size % 4096u != 0u);
}

static size_t heap_allocation_size(const void *pointer)
{
    if (!pointer) return 0;
    const union svga_test_heap_header *header = (const union svga_test_heap_header *)
        ((const uint8_t *)pointer - sizeof(union svga_test_heap_header));
    return header->allocation.requested_bytes;
}

static uint64_t eager_batch_sidecar_bytes(uint32_t capacity)
{
    return (uint64_t)capacity *
        (sizeof(struct svga_batch_query_op) +
         sizeof(struct svga_batch_shader_op) +
         sizeof(struct svga_batch_binding_op));
}

static uint64_t eager_batch_sidecar_pages(uint32_t capacity)
{
    return heap_pages_for_size((size_t)capacity * sizeof(struct svga_batch_query_op)) +
        heap_pages_for_size((size_t)capacity * sizeof(struct svga_batch_shader_op)) +
        heap_pages_for_size((size_t)capacity * sizeof(struct svga_batch_binding_op));
}

void *kernel_malloc(size_t size)
{
    if (assert_heap_unlocked) {
        uint64_t flags;
        assert(svga_try_lock_irqsave(&flags));
        svga_unlock(flags);
    }
    uint32_t call = heap_alloc_calls++;
    if (call == heap_fail_after) return NULL;
    if (size > SIZE_MAX - sizeof(union svga_test_heap_header)) return NULL;
    union svga_test_heap_header *header = malloc(
        sizeof(union svga_test_heap_header) + size);
    if (!header) return NULL;
    header->allocation.requested_bytes = size;
    ++heap_live_allocations;
    heap_live_bytes += size;
    heap_live_pages += heap_pages_for_size(size);
    return (uint8_t *)header + sizeof(*header);
}

void kernel_free(void *pointer)
{
    if (!pointer) return;
    if (assert_heap_unlocked) {
        uint64_t flags;
        assert(svga_try_lock_irqsave(&flags));
        svga_unlock(flags);
    }
    assert(heap_live_allocations > 0);
    union svga_test_heap_header *header = (union svga_test_heap_header *)
        ((uint8_t *)pointer - sizeof(union svga_test_heap_header));
    size_t size = header->allocation.requested_bytes;
    assert(heap_live_bytes >= size &&
           heap_live_pages >= heap_pages_for_size(size));
    --heap_live_allocations;
    heap_live_bytes -= size;
    heap_live_pages -= heap_pages_for_size(size);
    free(header);
}

void x86_64_outl(uint32_t value, uint16_t port)
{
    if (port == vmware_svga.io_port) {
        selected_port_reg = value;
    } else if (port == (uint16_t)(vmware_svga.io_port + 1u)) {
        if (selected_port_reg == SVGA_REG_TRACES) regs[SVGA_REG_TRACES] = value;
        if (selected_port_reg == SVGA_REG_SYNC) ++port_sync_writes;
    }
}

uint32_t x86_64_inl(uint16_t port)
{
    if (port == (uint16_t)(vmware_svga.io_port + 1u) &&
        selected_port_reg == SVGA_REG_BUSY) {
        ++port_busy_reads;
        return 1;
    }
    return 0;
}

void x86_64_outw(uint16_t value, uint16_t port)
{
    (void)value;
    (void)port;
}

uint16_t x86_64_inw(uint16_t port)
{
    (void)port;
    return 0;
}

void svga_platform_bind(uint16_t port, volatile uint32_t *ring, uint32_t bytes)
{
    (void)port;
    (void)ring;
    (void)bytes;
}

void console_log(enum console_log_level level, const char *format, ...)
{
    if (assert_console_log_unlocked) {
        uint64_t flags;
        assert(svga_try_lock_irqsave(&flags));
        svga_unlock(flags);
    }
    (void)level;
    va_list args;
    va_start(args, format);
    vsnprintf(console_log_text, sizeof(console_log_text), format, args);
    va_end(args);
    ++console_log_count;
}

static void *physical_pointer(uint64_t phys)
{
    device_ticks += 10000;
    for (uint32_t i = 0; i < 512; ++i)
        if (allocations[i].ptr && phys >= allocations[i].phys &&
            phys - allocations[i].phys < allocations[i].pages * 4096ULL)
            return (uint8_t *)allocations[i].ptr + phys - allocations[i].phys;
    return NULL;
}
static uint8_t *host_gmr_byte_pointer(uint32_t gmr, uint64_t offset)
{
    assert(gmr < SVGA_GMRS && offset / 4096u < host_gmr_pages[gmr]);
    uint32_t ppn = host_gmr_ppn[gmr][offset / 4096u];
    uint8_t *page = physical_pointer((uint64_t)ppn * 4096u);
    assert(page);
    return page + offset % 4096u;
}
static uint32_t host_surface_bytes_per_pixel(uint32_t format)
{
    return format == SVGA3D_ALPHA8 ? 1u : 4u;
}
static uint64_t read_clock(void) { return device_ticks; }
static void packet_copy(void *out, uint32_t base, uint32_t first, uint32_t bytes);
static uint32_t backing_page(struct host_backing b, uint32_t page)
{
    assert(page < (b.bytes + 4095u) / 4096u && b.depth <= 2);
    uint32_t ppn = b.root;
    if (b.depth == 2) {
        uint32_t *root = physical_pointer((uint64_t)ppn * 4096);
        assert(root);
        ppn = root[page / 1024];
    }
    if (b.depth) {
        uint32_t *leaf = physical_pointer((uint64_t)ppn * 4096);
        assert(leaf);
        ppn = leaf[page % 1024];
    }
    assert(physical_pointer((uint64_t)ppn * 4096));
    return ppn;
}
static void validate_backing(struct host_backing b)
{
    assert(b.bytes && b.depth <= 2 && (b.depth || b.bytes <= 4096));
    for (uint32_t i = 0; i < (b.bytes + 4095u) / 4096u; ++i) (void)backing_page(b, i);
}
static void check_not_referenced(struct host_backing b, uint64_t phys, uint32_t pages)
{
    if (!b.bytes) return;
    uint64_t start = phys >> 12, end = start + pages;
    assert(b.root < start || b.root >= end);
    if (b.depth == 2) {
        uint32_t *root = physical_pointer((uint64_t)b.root * 4096);
        assert(root);
        for (uint32_t i = 0; i < (b.bytes + 4194303u) / 4194304u; ++i)
            assert(root[i] < start || root[i] >= end);
    }
    for (uint32_t i = 0; i < (b.bytes + 4095u) / 4096u; ++i) {
        uint32_t ppn = backing_page(b, i);
        assert(ppn < start || ppn >= end);
    }
}
static void retire_backing(struct host_backing b)
{
    assert(retired_count < 512 && b.bytes);
    retired_backings[retired_count++] = b;
}
static uint32_t *table_entry(uint32_t type, uint32_t id, uint32_t stride)
{
    assert(type < 5 && (uint64_t)(id + 1) * stride <= host_otables[type].bytes);
    uint32_t offset = id * stride;
    return physical_pointer((uint64_t)backing_page(host_otables[type], offset / 4096) * 4096 + offset % 4096);
}
static uint64_t allocate(uint32_t pages)
{
    if (allocation_calls++ == fail_after) return 0;
    for (uint32_t i = 0; i < 512; ++i) if (!allocations[i].ptr) {
        void *p = calloc(pages, 4096);
        assert(p);
        uint64_t phys = next_phys;
        next_phys += pages * 4096ULL;
        allocations[i] = (struct allocation){phys, p, pages};
        live_pages += pages;
        return phys;
    }
    abort();
}
static void deallocate(uint64_t phys, uint32_t pages)
{
    if (phys == assert_unlocked_free_phys) {
        uint64_t flags;
        assert(svga_try_lock_irqsave(&flags));
        svga_unlock(flags);
        ++unlocked_free_checks;
        assert_unlocked_free_phys = 0;
    }
    for (uint32_t i = 0; i < 512; ++i) if (allocations[i].ptr && allocations[i].phys == phys) {
        assert(allocations[i].pages == pages);
        for (uint32_t id = 0; id < SVGA_GMRS; ++id)
            for (uint32_t p = 0; p < host_gmr_pages[id]; ++p)
                assert(host_gmr_ppn[id][p] < phys / 4096 || host_gmr_ppn[id][p] >= phys / 4096 + pages);
        for (uint32_t id = 0; id < SVGA_MOBS; ++id) check_not_referenced(host_mobs[id], phys, pages);
        for (uint32_t id = 0; id < 5; ++id) check_not_referenced(host_otables[id], phys, pages);
        for (uint32_t id = 0; id < retired_count; ++id) check_not_referenced(retired_backings[id], phys, pages);
        free(allocations[i].ptr);
        allocations[i] = (struct allocation){0};
        live_pages -= pages;
        return;
    }
    abort();
}
static uint32_t peek(uint32_t base, uint32_t word)
{
    uint32_t min = fifo[SVGA_FIFO_MIN], max = fifo[SVGA_FIFO_MAX];
    uint32_t offset = min + (base - min + word * 4) % (max - min);
    return fifo[offset / 4];
}
static void packet_copy(void *out, uint32_t base, uint32_t first, uint32_t bytes)
{
    uint32_t *words = out;
    for (uint32_t i = 0; i < bytes / 4; ++i) words[i] = peek(base, first + i);
}
static void consume_gb(uint32_t stop, uint32_t id, uint32_t size)
{
    /* Decode literal wire IDs and byte sizes independently of the driver. */
    uint32_t object = peek(stop, 2);
    assert(capabilities & SVGA_CAP_GBOBJECTS);
    switch (id) {
    case 1091: {
        assert(size == 20 && object < 5 && !peek(stop, 5));
        struct host_backing *b = &host_otables[object];
        if (!peek(stop, 4)) {
            assert(b->bytes && !peek(stop, 3) && peek(stop, 6) == UINT32_MAX);
            retire_backing(*b);
            *b = (struct host_backing){0};
        } else {
            assert(!b->bytes && peek(stop, 6) <= 1 && !(peek(stop, 4) & 4095));
            *b = (struct host_backing){peek(stop, 3), peek(stop, 6), peek(stop, 4)};
            validate_backing(*b);
        }
        break;
    }
    case 1093: {
        assert(size == 16 && object < SVGA_MOBS && !host_mobs[object].bytes);
        host_mobs[object] = (struct host_backing){peek(stop, 4), peek(stop, 3), peek(stop, 5)};
        validate_backing(host_mobs[object]);
        uint32_t *entry = table_entry(0, object, 16);
        entry[0] = peek(stop, 3); entry[1] = peek(stop, 5);
        entry[2] = peek(stop, 4); entry[3] = 0;
        break;
    }
    case 1094:
        assert(size == 4 && object < SVGA_MOBS && host_mobs[object].bytes);
        for (uint32_t i = 0; i < SVGA_CONTEXTS; ++i)
            assert(!host_contexts[i] || host_context_mobs[i] != object);
        for (uint32_t i = 0; i < SVGA_SURFACES; ++i)
            assert(!host_surfaces[i].gb || host_surfaces[i].mobid != object);
        for (uint32_t i = 0; i < SVGA_CONTEXTS; ++i)
            assert(!host_queries[i].pending || host_queries[i].mobid != object);
        retire_backing(host_mobs[object]);
        host_mobs[object] = (struct host_backing){0};
        memset(table_entry(0, object, 16), 0, 16);
        break;
    case 1107:
        assert(size == 4 && object < SVGA_CONTEXTS && !host_contexts[object]);
        host_contexts[object] = true;
        host_context_mobs[object] = UINT32_MAX;
        table_entry(2, object, 8)[0] = object;
        break;
    case 1109: {
        assert(size == 12 && object < SVGA_CONTEXTS && host_contexts[object] && !peek(stop, 4));
        uint32_t mob = peek(stop, 3);
        if (mob != UINT32_MAX) assert(mob < SVGA_MOBS && host_mobs[mob].bytes >= 16384);
        host_context_mobs[object] = mob;
        table_entry(2, object, 8)[1] = mob;
        break;
    }
    case 1108:
        assert(size == 4 && object < SVGA_CONTEXTS && host_contexts[object]);
        assert(host_context_mobs[object] == UINT32_MAX);
        assert(!host_queries[object].active && !host_queries[object].pending);
        host_contexts[object] = false;
        memset(table_entry(2, object, 8), 0, 8);
        break;
    case 1112: {
        assert(size == 12 && object < SVGA_GB_SHADER_SLOTS &&
               !host_shaders[object].defined);
        uint32_t type = peek(stop, 3), bytes = peek(stop, 4);
        assert((type == SVGA3D_SHADERTYPE_VS || type == SVGA3D_SHADERTYPE_PS) && bytes);
        host_shaders[object] = (struct host_shader){type, bytes, UINT32_MAX, true};
        uint32_t *entry = table_entry(SVGA_OTABLE_SHADER, object,
                                      SVGA_GB_SHADER_ENTRY_BYTES);
        entry[0] = type; entry[1] = bytes; entry[2] = 0; entry[3] = UINT32_MAX;
        break;
    }
    case 1114: {
        assert(size == 12 && object < SVGA_GB_SHADER_SLOTS &&
               host_shaders[object].defined);
        uint32_t mob = peek(stop, 3), offset = peek(stop, 4);
        uint32_t *entry = table_entry(SVGA_OTABLE_SHADER, object,
                                      SVGA_GB_SHADER_ENTRY_BYTES);
        if (mob == UINT32_MAX) {
            assert(!offset && host_shaders[object].mobid != UINT32_MAX);
        } else {
            assert(mob < SVGA_MOBS && host_mobs[mob].bytes &&
                   offset <= host_mobs[mob].bytes &&
                   host_shaders[object].bytes <= host_mobs[mob].bytes - offset);
        }
        host_shaders[object].mobid = mob;
        entry[2] = offset; entry[3] = mob;
        break;
    }
    case 1113: {
        assert(size == 4 && object < SVGA_GB_SHADER_SLOTS &&
               host_shaders[object].defined &&
               host_shaders[object].mobid == UINT32_MAX);
        host_shaders[object] = (struct host_shader){0};
        memset(table_entry(SVGA_OTABLE_SHADER, object,
                           SVGA_GB_SHADER_ENTRY_BYTES), 0,
               SVGA_GB_SHADER_ENTRY_BYTES);
        break;
    }
    case 1116:
        assert(size == 8 && object < SVGA_CONTEXTS && host_contexts[object] &&
               peek(stop, 3) == SVGA_GB_QUERY_TYPE_OCCLUSION &&
               !host_queries[object].active);
        host_queries[object].active = true;
        break;
    case 1117: {
        assert(size == 16 && object < SVGA_CONTEXTS && host_contexts[object] &&
               host_queries[object].active &&
               peek(stop, 3) == SVGA_GB_QUERY_TYPE_OCCLUSION);
        uint32_t mobid = peek(stop, 4), offset = peek(stop, 5);
        assert(mobid < SVGA_MOBS && host_mobs[mobid].bytes >= SVGA_GB_QUERY_RESULT_BYTES &&
               offset <= host_mobs[mobid].bytes - SVGA_GB_QUERY_RESULT_BYTES);
        uint32_t ppn = backing_page(host_mobs[mobid], offset / 4096u);
        SVGA3dQueryResult *result = physical_pointer((uint64_t)ppn * 4096 + offset % 4096u);
        assert(result);
        result->totalSize = SVGA_GB_QUERY_RESULT_BYTES;
        result->state = SVGA3D_QUERYSTATE_SUCCEEDED;
        result->result32 = 5;
        host_queries[object] = (struct host_query){mobid, offset, false, true};
        break;
    }
    case 1118: {
        assert(size == 16 && object < SVGA_CONTEXTS && host_contexts[object] &&
               peek(stop, 3) == SVGA_GB_QUERY_TYPE_OCCLUSION);
        uint32_t mobid = peek(stop, 4), offset = peek(stop, 5);
        assert(host_queries[object].pending && host_queries[object].mobid == mobid &&
               host_queries[object].offset == offset);
        uint32_t ppn = backing_page(host_mobs[mobid], offset / 4096u);
        SVGA3dQueryResult *result = physical_pointer((uint64_t)ppn * 4096 + offset % 4096u);
        assert(result && result->totalSize == SVGA_GB_QUERY_RESULT_BYTES &&
               result->state == SVGA3D_QUERYSTATE_SUCCEEDED);
        host_queries[object].pending = false;
        break;
    }
    case 1097: {
        assert(size == 36 && object < SVGA_SURFACES && !host_surfaces[object].data);
        struct host_surface *s = &host_surfaces[object];
        s->format = peek(stop, 4); s->width = peek(stop, 8); s->height = peek(stop, 9);
        assert(peek(stop, 5) && !peek(stop, 6) && !peek(stop, 7) && peek(stop, 10));
        s->data = calloc((size_t)s->width * s->height,
                         host_surface_bytes_per_pixel(s->format));
        assert(s->data);
        s->gb = true; s->mobid = UINT32_MAX;
        table_entry(1, object, 64)[0] = s->format;
        break;
    }
    case 1099: {
        assert(size == 8 && object < SVGA_SURFACES && host_surfaces[object].gb);
        uint32_t mob = peek(stop, 3);
        if (mob != UINT32_MAX) assert(mob < SVGA_MOBS && host_mobs[mob].bytes);
        host_surfaces[object].mobid = mob;
        table_entry(1, object, 64)[8] = mob;
        break;
    }
    case 1102:
    case 1106:
        assert(size == 4 && object < SVGA_SURFACES && host_surfaces[object].gb);
        assert(host_surfaces[object].mobid != UINT32_MAX);
        break;
    case 1098:
        assert(size == 4 && object < SVGA_SURFACES && host_surfaces[object].gb);
        assert(host_surfaces[object].mobid == UINT32_MAX);
        free(host_surfaces[object].data);
        host_surfaces[object] = (struct host_surface){0};
        memset(table_entry(1, object, 64), 0, 64);
        break;
    default: fprintf(stderr, "Unexpected GB FIFO command %u\n", id); abort();
    }
}
static void consume(void)
{
    if (stalled || !regs[SVGA_REG_CONFIG_DONE]) return;
    uint32_t stop = fifo[SVGA_FIFO_STOP], next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t min = fifo[SVGA_FIFO_MIN], max = fifo[SVGA_FIFO_MAX];
    while (stop != next) {
        uint32_t available = next >= stop ? next - stop : max - stop + next - min;
        uint32_t id = peek(stop, 0), words = 0;
        if (stall_command == id) { stall_command = 0; stalled = true; return; }
        if ((id >= 1091 && id <= 1114) || (id >= 1116 && id <= 1118)) {
            assert(available >= 8 && !(peek(stop, 1) & 3));
            words = 2 + peek(stop, 1) / 4;
            consume_gb(stop, id, peek(stop, 1));
        } else if (id >= 1040 && id < 1082) {
            assert(available >= 8);
            uint32_t size = peek(stop, 1);
            assert(!(size & 3));
            words = 2 + size / 4;
            if (id == SVGA_3D_CMD_CONTEXT_DEFINE || id == SVGA_3D_CMD_CONTEXT_DESTROY ||
                id == SVGA_3D_CMD_SURFACE_DESTROY) assert(size == 4);
            if (id == SVGA_3D_CMD_SETRENDERSTATE) {
                assert(size >= sizeof(SVGA3dCmdSetRenderState) + sizeof(SVGA3dRenderState));
                uint32_t cid = peek(stop, 2);
                for (uint32_t word = 3; word < 2 + size / 4; word += 2) {
                    uint32_t state = peek(stop, word), value = peek(stop, word + 1);
                    if (state == SVGA3D_RS_ZENABLE) host_depth_enabled[cid] = value;
                    if (state == SVGA3D_RS_FILLMODE) host_fill[cid] = value;
                    if (state == SVGA3D_RS_CULLMODE) assert(value == SVGA3D_FACE_NONE);
                }
            }
            if (id == SVGA_3D_CMD_SETRENDERTARGET) assert(size == sizeof(SVGA3dCmdSetRenderTarget));
            if (id == SVGA_3D_CMD_SET_SHADER) {
                assert(size == sizeof(SVGA3dCmdSetShader));
                uint32_t cid = peek(stop, 2), type = peek(stop, 3), shid = peek(stop, 4);
                assert(cid < SVGA_CONTEXTS && type >= SVGA3D_SHADERTYPE_VS &&
                       type <= SVGA3D_SHADERTYPE_PS);
                if (shid != SVGA3D_INVALID_ID)
                    assert(shid < SVGA_GB_SHADER_SLOTS && host_shaders[shid].defined);
                host_shaders_bound[cid][type - SVGA3D_SHADERTYPE_VS] = shid;
            }
            if (id == SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE) {
                assert(size >= 32 && ((size - 16) % 16) == 0);
                uint32_t type = peek(stop, 4), const_type = peek(stop, 5);
                uint32_t start = peek(stop, 3), count = (size - 16) / 16;
                uint32_t limit = const_type == SVGA3D_CONST_TYPE_FLOAT ? 256u : 16u;
                assert(type >= SVGA3D_SHADERTYPE_VS && type <= SVGA3D_SHADERTYPE_PS);
                assert(const_type <= SVGA3D_CONST_TYPE_BOOL && start < limit &&
                       count <= limit - start);
            }
            if (id == SVGA_3D_CMD_SETVIEWPORT) assert(size == sizeof(SVGA3dCmdSetViewport));
            if (id == SVGA_3D_CMD_CLEAR) {
                assert(size == sizeof(SVGA3dCmdClear) + sizeof(SVGA3dRect));
                assert(peek(stop, 7) == 0 && peek(stop, 8) == 0);
                assert(peek(stop, 9) && peek(stop, 10));
                uint32_t cid = peek(stop, 2), sid = host_targets[cid][SVGA3D_RT_COLOR0];
                struct host_surface *surface = &host_surfaces[sid];
                assert(peek(stop, 9) == surface->width && peek(stop, 10) == surface->height);
                for (uint32_t p = 0; p < surface->width * surface->height; ++p)
                    ((uint32_t *)surface->data)[p] = peek(stop, 4);
            }
            if (id == SVGA_3D_CMD_SETRENDERTARGET)
                host_targets[peek(stop, 2)][peek(stop, 3)] = peek(stop, 4);
            if (id == SVGA_3D_CMD_SETTRANSFORM) {
                assert(size == sizeof(SVGA3dCmdSetTransform));
                uint32_t type = peek(stop, 3), cid = peek(stop, 2);
                if (type == SVGA3D_TRANSFORM_PROJECTION)
                    packet_copy(host_projection[cid], stop, 4, sizeof(host_projection[cid]));
                else {
                    assert(type == SVGA3D_TRANSFORM_WORLD || type == SVGA3D_TRANSFORM_VIEW);
                    for (uint32_t i = 0; i < 16; ++i)
                        assert(peek(stop, 4 + i) == (i % 5 == 0 ? 0x3f800000u : 0));
                }
                ++transform_count;
            }
            if (id == SVGA_3D_CMD_SETZRANGE) {
                assert(size == sizeof(SVGA3dCmdSetZRange));
                assert(peek(stop, 3) == 0 && peek(stop, 4) == 0x3f800000u);
            }
            if (id == SVGA_3D_CMD_SURFACE_DMA) {
                assert(size >= sizeof(SVGA3dCmdSurfaceDMA) + sizeof(SVGA3dCopyBox));
                SVGA3dCmdSurfaceDMA dma;
                SVGA3dCopyBox box;
                packet_copy(&dma, stop, 2, sizeof(dma));
                packet_copy(&box, stop, 2 + sizeof(dma) / 4, sizeof(box));
                struct host_surface *surface = &host_surfaces[dma.host.sid];
                if (surface->format == SVGA3D_ALPHA8) {
                    assert(surface->data && box.d == 1 &&
                           box.x <= surface->width &&
                           box.w <= surface->width - box.x &&
                           box.y <= surface->height &&
                           box.h <= surface->height - box.y &&
                           dma.guest.pitch >= box.srcx + box.w);
                    for (uint32_t y = 0; y < box.h; ++y) {
                        uint64_t guest_row = dma.guest.ptr.offset +
                            (uint64_t)(box.srcy + y) * dma.guest.pitch + box.srcx;
                        uint64_t host_row =
                            (uint64_t)(box.y + y) * surface->width + box.x;
                        for (uint32_t x = 0; x < box.w; ++x) {
                            uint8_t *guest = host_gmr_byte_pointer(
                                dma.guest.ptr.gmrId, guest_row + x);
                            if (dma.transfer == SVGA3D_WRITE_HOST_VRAM)
                                surface->data[host_row + x] = *guest;
                            else {
                                assert(dma.transfer == SVGA3D_READ_HOST_VRAM);
                                *guest = surface->data[host_row + x];
                            }
                        }
                    }
                } else if (dma.transfer == SVGA3D_READ_HOST_VRAM) {
                    assert(surface->data && box.w == surface->width && box.h == surface->height);
                    assert(!box.x && !box.y && !box.z && box.d == 1);
                    assert(!box.srcx && !box.srcy && !box.srcz && !dma.guest.ptr.offset);
                    assert(dma.guest.pitch == surface->width * 4);
                    for (uint32_t offset = 0; offset < box.w * box.h * 4; ++offset) {
                        uint32_t ppn = host_gmr_ppn[dma.guest.ptr.gmrId][offset / 4096];
                        uint8_t *guest = physical_pointer((uint64_t)ppn * 4096);
                        assert(guest);
                        guest[offset % 4096] = surface->data[offset];
                    }
                    ++readback_count;
                }
                if (peek(stop, 4) == 60 && peek(stop, 8) == SVGA3D_WRITE_HOST_VRAM) {
                    uint32_t gmr = peek(stop, 2), offset = peek(stop, 3);
                    assert(gmr < SVGA_GMRS && host_gmr_pages[gmr] && offset == 0);
                    void *data = physical_pointer((uint64_t)host_gmr_ppn[gmr][0] * 4096);
                    struct { float x, y, z, rhw; uint32_t color; } vertices[3];
                    assert(data && sizeof(vertices) == 60);
                    memcpy(vertices, data, sizeof(vertices));
                    uint32_t width = regs[SVGA_REG_WIDTH], height = regs[SVGA_REG_HEIGHT];
                    assert(vertices[0].x == (float)(width / 4u));
                    assert(vertices[1].x == (float)((uint64_t)width * 3u / 4u));
                    assert(vertices[2].x == (float)(width / 2u));
                    assert(vertices[0].y == (float)((uint64_t)height * 17u / 100u));
                    assert(vertices[1].y == vertices[0].y);
                    assert(vertices[2].y == (float)((uint64_t)height * 83u / 100u));
                    const uint32_t colors[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu};
                    for (uint32_t v = 0; v < 3; ++v) {
                        assert(vertices[v].z == 0.5f && vertices[v].rhw == 1.0f);
                        assert(vertices[v].color == colors[v]);
                    }
                    ++triangle_uploads;
                }
            }
            if (id == SVGA_3D_CMD_SURFACE_DEFINE) {
                assert(!(capabilities & SVGA_CAP_GBOBJECTS));
                SVGA3dCmdDefineSurface define;
                uint32_t faces, levels;
                uint32_t *define_words = (uint32_t *)&define;
                for (uint32_t word = 0; word < sizeof(define) / 4; ++word)
                    define_words[word] = peek(stop, 2 + word);
                faces = (define.surfaceFlags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u;
                levels = define.face[0].numMipLevels;
                assert(levels && size == sizeof(define) + faces * levels * sizeof(SVGA3dSize));
                struct host_surface *surface = &host_surfaces[define.sid];
                assert(!surface->data);
                surface->format = define.format;
                surface->width = peek(stop, 2 + sizeof(define) / 4);
                surface->height = peek(stop, 3 + sizeof(define) / 4);
                surface->data = calloc((size_t)surface->width * surface->height,
                    host_surface_bytes_per_pixel(surface->format));
                assert(surface->data);
            }
            if (id == SVGA_3D_CMD_SURFACE_DESTROY) {
                assert(!(capabilities & SVGA_CAP_GBOBJECTS));
                struct host_surface *surface = &host_surfaces[peek(stop, 2)];
                free(surface->data);
                *surface = (struct host_surface){0};
            }
            if (id == SVGA_3D_CMD_DRAW_PRIMITIVES) {
                assert(size >= sizeof(SVGA3dCmdDrawPrimitives) +
                       2 * sizeof(SVGA3dVertexDecl) + sizeof(SVGA3dPrimitiveRange));
                ++draw_count;
                SVGA3dVertexDecl decl;
                packet_copy(&decl, stop, 2 + sizeof(SVGA3dCmdDrawPrimitives) / 4, sizeof(decl));
                if (decl.identity.type == SVGA3D_DECLTYPE_FLOAT3) {
                    assert(decl.identity.usage == SVGA3D_DECLUSAGE_POSITION);
                    assert(decl.array.stride == sizeof(struct reliefos_gpu_vertex));
                    uint32_t cid = peek(stop, 2), sid = host_targets[cid][SVGA3D_RT_COLOR0];
                    struct host_surface *surface = &host_surfaces[sid];
                    assert(host_targets[cid][SVGA3D_RT_DEPTH] != SVGA3D_INVALID_ID);
                    assert(host_depth_enabled[cid] && host_fill[cid] >= 1 && host_fill[cid] <= 3);
                    SVGA3dVertexDecl color;
                    SVGA3dPrimitiveRange range;
                    packet_copy(&color, stop, 2 + (sizeof(SVGA3dCmdDrawPrimitives) + sizeof(decl)) / 4,
                                sizeof(color));
                    packet_copy(&range, stop, 2 + (sizeof(SVGA3dCmdDrawPrimitives) + 2 * sizeof(decl)) / 4,
                                sizeof(range));
                    assert(color.identity.type == SVGA3D_DECLTYPE_D3DCOLOR);
                    assert(color.identity.usage == SVGA3D_DECLUSAGE_COLOR && color.array.offset == 12);
                    assert(range.primType == SVGA3D_PRIMITIVE_TRIANGLELIST && range.primitiveCount == 1);
                    assert(range.indexArray.surfaceId == SVGA3D_INVALID_ID);
                    assert(range.indexBias == 0 || range.indexBias == 3);
                    assert(host_projection[cid][12] == (range.indexBias ? 0x3f000000u : 0x3e800000u));
                    assert(host_projection[cid][3] == 0);
                    ((uint32_t *)surface->data)[0] = 0xff112233;
                    ((uint32_t *)surface->data)[surface->width * surface->height - 1] = 0xffaabbcc;
                    ++depth_draw_count;
                }
            }
            if (id == SVGA_3D_CMD_PRESENT) {
                assert(size >= 28); ++present_count;
                if (!host_otables[4].bytes) ++visible_presents;
            }
        } else if (id == SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE) {
            assert(available >= 8);
            uint32_t size = peek(stop, 1);
            assert(size >= 32 && ((size - 16) % 16) == 0);
            uint32_t type = peek(stop, 4), const_type = peek(stop, 5);
            uint32_t start = peek(stop, 3), count = (size - 16) / 16;
            uint32_t limit = const_type == SVGA3D_CONST_TYPE_FLOAT ? 256u : 16u;
            assert(type >= SVGA3D_SHADERTYPE_VS && type <= SVGA3D_SHADERTYPE_PS);
            assert(const_type <= SVGA3D_CONST_TYPE_BOOL && start < limit &&
                   count <= limit - start);
            words = 2 + size / 4;
        } else if (id == SVGA_CMD_FENCE) {
            words = 2;
            fifo[SVGA_FIFO_FENCE] = peek(stop, 1);
            retired_count = 0;
        } else if (id == SVGA_CMD_UPDATE) {
            words = 5;
            /* Installing the Screen Target table switches away from the
             * screen-object/legacy UPDATE and PRESENT display interface. */
            if (!host_otables[4].bytes) ++visible_updates;
        }
        else if (id == SVGA_CMD_DEFINE_GMR2) {
            words = 3;
            uint32_t gmr = peek(stop, 1), pages = peek(stop, 2);
            assert(gmr < SVGA_GMRS && pages <= SVGA_MAX_GUEST_PAGES);
            host_gmr_pages[gmr] = pages;
            memset(host_gmr_ppn[gmr], 0, sizeof(host_gmr_ppn[gmr]));
        } else if (id == SVGA_CMD_REMAP_GMR2) {
            uint32_t gmr = peek(stop, 1), flags = peek(stop, 2), offset = peek(stop, 3), count = peek(stop, 4);
            assert(gmr < SVGA_GMRS && flags == SVGA_REMAP_GMR2_PPN32);
            assert(offset <= host_gmr_pages[gmr] && count <= host_gmr_pages[gmr] - offset);
            words = 5 + count;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t ppn = peek(stop, 5 + i);
                assert(physical_pointer((uint64_t)ppn * 4096));
                host_gmr_ppn[gmr][offset + i] = ppn;
            }
        } else { fprintf(stderr, "Unexpected FIFO command %u\n", id); abort(); }
        assert(words * 4 <= available);
        ++command_count;
        device_ticks += 100;
        stop = min + (stop - min + words * 4) % (max - min);
        fifo[SVGA_FIFO_STOP] = stop;
    }
    if (host_busy_supported)
        fifo[SVGA_FIFO_BUSY] = 0;
}
static uint32_t read_reg(uint32_t reg)
{
    if (reg == SVGA_REG_BUSY) {
        ++busy_reads;
        if (!host_delay_reads || !--host_delay_reads) consume();
        return stalled || fifo[SVGA_FIFO_STOP] != fifo[SVGA_FIFO_NEXT_CMD];
    }
    if (reg == SVGA_REG_DEV_CAP) {
        assert(capabilities & SVGA_CAP_GBOBJECTS);
        ++devcap_queries;
        switch (regs[reg]) {
        case SVGA3D_DEVCAP_3D: return host_devcap_3d;
        case SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH: return host_texture_width_cap;
        case SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT: return host_texture_height_cap;
        case SVGA3D_DEVCAP_MAX_VOLUME_EXTENT: return 256;
        case SVGA3D_DEVCAP_MAX_CONTEXT_IDS: return host_context_cap;
        case SVGA3D_DEVCAP_MAX_SURFACE_IDS: return host_surface_cap;
        case SVGA3D_DEVCAP_MAX_RENDER_TARGETS:
        case SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS:
            return host_render_targets_cap;
        case SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8:
            return SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
                   SVGA3DFORMAT_OP_CUBETEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                   SVGA3DFORMAT_OP_DISPLAYMODE | SVGA3DFORMAT_OP_SRGBREAD;
        case SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8:
            return SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
                   SVGA3DFORMAT_OP_CUBETEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                   SVGA3DFORMAT_OP_SRGBREAD;
        case SVGA3D_DEVCAP_SURFACEFMT_Z_D16:
        case SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8:
            return SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL;
        default: return 0;
        }
    }
    return regs[reg];
}
static void write_reg(uint32_t reg, uint32_t value)
{
    regs[reg] = value;
    if (reg == SVGA_REG_CONFIG_DONE && !value) regs[SVGA_REG_TRACES] = 0;
    if (reg == SVGA_REG_CONFIG_DONE && value) {
        host_busy_supported =
            (regs[SVGA_REG_CAPABILITIES] & SVGA_CAP_EXTENDED_FIFO) &&
            regs[SVGA_REG_MEM_REGS] > SVGA_FIFO_BUSY &&
            fifo[SVGA_FIFO_MIN] > SVGA_FIFO_BUSY * 4u &&
            fifo[SVGA_FIFO_MIN] <= sizeof(fifo);
        if (fifo[SVGA_FIFO_MIN] > SVGA_FIFO_GUEST_3D_HWVERSION * 4) {
            configured_guest = fifo[SVGA_FIFO_GUEST_3D_HWVERSION];
            fifo[SVGA_FIFO_CAPABILITIES] = fifo_caps;
            fifo[SVGA_FIFO_3D_HWVERSION] = host_version;
            fifo[SVGA_FIFO_3D_HWVERSION_REVISED] = host_version;
            uint32_t p = SVGA_FIFO_3D_CAPS;
            const uint32_t caps[] = {SVGA3D_DEVCAP_3D, 1,
                SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH, 4096, SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT, 4096,
                SVGA3D_DEVCAP_MAX_VOLUME_EXTENT, 256,
                SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8,
                    SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
                    SVGA3DFORMAT_OP_CUBETEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                    SVGA3DFORMAT_OP_DISPLAYMODE | SVGA3DFORMAT_OP_SRGBREAD,
                SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8,
                    SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
                    SVGA3DFORMAT_OP_CUBETEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                    SVGA3DFORMAT_OP_SRGBREAD,
                SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8,
                    SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL,
                SVGA3D_DEVCAP_SURFACEFMT_Z_D16,
                    SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL,
                SVGA3D_DEVCAP_SURFACEFMT_DXT1, 0xffffffff,
                SVGA3D_DEVCAP_MAX_CONTEXT_IDS, SVGA_CONTEXTS,
                SVGA3D_DEVCAP_MAX_SURFACE_IDS, SVGA_SURFACES};
            fifo[p++] = 2 + sizeof(caps) / 4; fifo[p++] = 0x100;
            for (uint32_t i = 0; i < sizeof(caps) / 4; ++i) fifo[p++] = caps[i];
            fifo[p] = 0;
            if (invalid_caps_record) fifo[SVGA_FIFO_3D_CAPS] = 300;
        }
    }
    if (reg == SVGA_REG_SYNC) {
        ++sync_writes;
        if (host_busy_supported) {
            if (expect_guest_fifo_busy) {
                /* The guest must publish BUSY before ringing the doorbell. */
                assert(fifo[SVGA_FIFO_BUSY] == 1);
                expect_guest_fifo_busy = false;
            }
            fifo[SVGA_FIFO_BUSY] = 1;
        }
        if (!host_delay_reads) consume();
    }
    if (reg == SVGA_REG_GMR_ID) selected_gmr = value;
    if (reg == SVGA_REG_GMR_DESCRIPTOR) {
        assert(selected_gmr < SVGA_GMRS);
        if (!value) host_gmr_pages[selected_gmr] = 0;
        else {
            SVGAGuestMemDescriptor *desc = physical_pointer((uint64_t)value * 4096);
            assert(desc && desc[1].ppn == 0 && desc[1].numPages == 0);
            assert(desc[0].numPages <= SVGA_MAX_GUEST_PAGES);
            host_gmr_pages[selected_gmr] = desc[0].numPages;
            for (uint32_t i = 0; i < desc[0].numPages; ++i)
                host_gmr_ppn[selected_gmr][i] = desc[0].ppn + i;
        }
    }
}
static void setup(uint32_t caps, uint32_t fc, uint32_t version)
{
    assert(live_pages == 0);
    assert(!retired_count);
    assert(!heap_live_allocations);
    assert(!heap_live_bytes && !heap_live_pages);
    memset(&svga, 0, sizeof(svga)); memset(fifo, 0, sizeof(fifo)); memset(regs, 0, sizeof(regs));
    memset(host_gmr_pages, 0, sizeof(host_gmr_pages));
    memset(host_shaders, 0, sizeof(host_shaders));
    memset(host_shaders_bound, 0xff, sizeof(host_shaders_bound));
    memset(host_queries, 0, sizeof(host_queries));
    capabilities = caps; fifo_caps = fc; host_version = version;
    regs[SVGA_REG_CAPABILITIES] = capabilities;
    regs[SVGA_REG_MEM_REGS] = SVGA_FIFO_NUM_REGS;
    regs[SVGA_REG_GMR_MAX_IDS] = SVGA_GMRS;
    regs[SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH] = 512;
    regs[SVGA_REG_GMRS_MAX_PAGES] = SVGA_MAX_GUEST_PAGES;
    regs[SVGA_REG_VRAM_SIZE] = 16 * 1024 * 1024;
    regs[SVGA_REG_MEMORY_SIZE] = 256 * 1024 * 1024;
    regs[SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB] = 256 * 1024;
    regs[SVGA_REG_MOB_MAX_SIZE] = 128 * 1024 * 1024;
    regs[SVGA_REG_WIDTH] = 800; regs[SVGA_REG_HEIGHT] = 600;
    allocation_calls = 0; fail_after = UINT32_MAX; stalled = false; stall_command = 0;
    heap_alloc_calls = 0; heap_fail_after = UINT32_MAX;
    invalid_caps_record = false;
    host_devcap_3d = 1;
    devcap_queries = 0;
    host_delay_reads = 0;
    expect_guest_fifo_busy = false;
    const struct svga_ops ops = {read_reg, write_reg, allocate, deallocate, physical_pointer, read_clock};
    svga_bind(&ops, fifo, sizeof(fifo));
    svga.memory_ready = true;
    assert(svga_configure_locked(true) == 0);
}
static void test_fifo(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    assert(svga.available && configured_guest == SVGA3D_HWVERSION_WS8_B1);
    /* A normal scanout update is asynchronous: a stalled host must not turn
     * the framebuffer present path into a bounded BUSY wait. */
    stalled = true;
    fifo[SVGA_FIFO_NEXT_CMD] = fifo[SVGA_FIFO_STOP] = svga.min;
    uint32_t busy_before = busy_reads;
    assert(svga_update(1, 2, 3, 4) == 0);
    assert(busy_reads == busy_before);
    assert(fifo[SVGA_FIFO_NEXT_CMD] != fifo[SVGA_FIFO_STOP]);
    stalled = false;
    consume();
    for (uint32_t reserve = 0; reserve < 2; ++reserve) {
        svga.fifo_caps = SVGA_FIFO_CAP_FENCE | (reserve ? SVGA_FIFO_CAP_RESERVE : 0);
        for (uint32_t tail = 4; tail <= 20; tail += 4) {
            fifo[SVGA_FIFO_NEXT_CMD] = fifo[SVGA_FIFO_STOP] = FIFO_BYTES - tail;
            assert(svga_update(1, 2, 3, 4) == 0);
            assert(fifo[SVGA_FIFO_NEXT_CMD] == fifo[SVGA_FIFO_STOP]);
        }
    }
    fifo[SVGA_FIFO_NEXT_CMD] = svga.min;
    fifo[SVGA_FIFO_STOP] = svga.min + 24;
    stalled = true;
    uint32_t update[] = {1, 2, 3, 4};
    struct svga_span span = {update, 16};
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    uint32_t before = fifo[SVGA_FIFO_NEXT_CMD];
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == SVGA_ETIMEDOUT);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == before);
    fifo[SVGA_FIFO_NEXT_CMD] = svga.min + 1;
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == SVGA_EIO);
    stalled = false;
    fifo[SVGA_FIFO_NEXT_CMD] = fifo[SVGA_FIFO_STOP] = svga.min;
    svga.next_fence = 0xfffffffe;
    fifo[SVGA_FIFO_FENCE] = 0xfffffffd;
    for (uint32_t i = 0; i < 4; ++i) {
        svga_handle f; assert(!svga_fence_insert(&f)); assert(!svga_fence_wait(f));
    }
    struct svga_fence_progress progress;
    assert(!svga_fence_progress_snapshot(&progress));
    assert(progress.generation == svga.generation &&
           progress.issued_seq == 2u && progress.passed_seq == 2u);
    bool signaled = true;
    uint32_t unissued = progress.issued_seq + 1u;
    assert(svga_fence_status(((uint64_t)progress.generation << 32) | unissued,
                             &signaled) == SVGA_EINVAL && !signaled);
    stalled = true;
    svga_handle f; assert(!svga_fence_insert(&f));
    assert(svga_fence_wait(f) == SVGA_ETIMEDOUT);
    assert(!svga_fence_progress_snapshot(&progress));
    assert(progress.issued_seq == 3u && progress.passed_seq == 2u);
    stalled = false; consume();
    assert(!svga_fence_progress_snapshot(&progress));
    assert(progress.issued_seq == 3u && progress.passed_seq == 3u);
    assert(!svga3d_shutdown());
}

static void test_nonblocking_batch(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            sizeof(batch_storage)));
    const uint32_t update_a[] = {1, 2, 3, 4};
    const uint32_t update_b[] = {5, 6, 7, 8};
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update_a, sizeof(update_a)));
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update_b, sizeof(update_b)));
    const uint32_t staged_bytes = batch.bytes;
    const uint32_t next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    svga.fifo[SVGA_FIFO_STOP] = next + 32;
    stalled = true;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EBUSY);
    assert(!fence && !batch.submitted && batch.bytes == staged_bytes);
    assert(svga.fifo[SVGA_FIFO_NEXT_CMD] == next);

    svga.fifo[SVGA_FIFO_STOP] = next;
    stalled = false;
    /* The preceding backpressure path already notified the host. */
    consume();
    uint32_t before_commands = command_count;
    assert(!svga_batch_submit(&batch, &fence) && fence);
    assert(batch.submitted && svga.fifo[SVGA_FIFO_STOP] == svga.fifo[SVGA_FIFO_NEXT_CMD]);
    assert(command_count == before_commands + 3);
    bool signaled = false;
    assert(!svga_fence_status(fence, &signaled) && signaled);
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL);
    ++svga.generation;
    assert(svga_fence_status(fence, &signaled) == SVGA_EIO);
    --svga.generation;
}

static void test_mode_lease_preserves_live_backend(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    uint32_t generation = svga.generation;
    uint32_t pages = live_pages;
    assert(!svga_mode_lease_acquire());
    uint64_t flags = 0;
    assert(svga_mode_begin(&flags) == SVGA_EBUSY);
    assert(svga.available && svga.fifo_ready && svga.generation == generation);
    assert(live_pages == pages && svga.mode_leases == 1);
    assert(!svga_mode_lease_release());
    assert(svga_mode_lease_release() == SVGA_EINVAL);
    assert(!svga_mode_begin(&flags));
    svga_mode_end(flags);
    assert(svga.available && svga.generation != generation);
    assert(!svga3d_shutdown());
}

static void test_fifo_wakeup(void)
{
    uint32_t update[] = {0, 0, 10, 10};
    struct svga_span span = {update, sizeof(update)};
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    fifo[SVGA_FIFO_BUSY] = 1;
    assert(svga_configure_locked(true) == 0 && !fifo[SVGA_FIFO_BUSY]);
    uint32_t wakes = sync_writes, updates = visible_updates;
    stalled = true;
    expect_guest_fifo_busy = true;
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(!expect_guest_fifo_busy);
    assert(sync_writes == wakes + 1);
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(sync_writes == wakes + 2);
    svga_handle fence;
    assert(svga_fence_insert(&fence) == 0);
    assert(sync_writes == wakes + 3);
    assert(fifo[SVGA_FIFO_BUSY] == 1);
    stalled = false;
    consume();
    assert(visible_updates == updates + 2 && !fifo[SVGA_FIFO_BUSY]);
    uint32_t reads = busy_reads;
    wakes = sync_writes;
    assert(svga_fence_wait(fence) == 0);
    assert(sync_writes == wakes + 1 && busy_reads == reads);
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(sync_writes == wakes + 2 && visible_updates == updates + 3);
    assert(svga3d_shutdown() == 0);

    /* Four-register FIFO: never treat a command word as the BUSY register. */
    setup(0, 0, 0);
    assert(svga.min == 16);
    wakes = sync_writes;
    updates = visible_updates;
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(sync_writes == wakes + 2 && visible_updates == updates + 2);
}

static void test_fifo_busy_requires_reported_register(void)
{
    uint32_t update[] = {0, 0, 10, 10};
    struct svga_span span = {update, sizeof(update)};
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);

    /* MEM_REGS is a count: BUSY is absent when the raw count equals its index,
     * even though configure clamps the allocated header up to NUM_REGS. */
    regs[SVGA_REG_MEM_REGS] = SVGA_FIFO_BUSY;
    const uint32_t busy_sentinel = 0xdecafbad;
    fifo[SVGA_FIFO_BUSY] = busy_sentinel;
    uint32_t wakes = sync_writes;
    assert(svga_configure_locked(true) == 0);
    assert(!host_busy_supported && fifo[SVGA_FIFO_BUSY] == busy_sentinel);

    stalled = true;
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(sync_writes == wakes + 1);
    assert(fifo[SVGA_FIFO_BUSY] == busy_sentinel);
    stalled = false;
    consume();
}

static void test_fifo_busy_state_resets_on_reconfigure(void)
{
    uint32_t update[] = {0, 0, 10, 10};
    struct svga_span span = {update, sizeof(update)};
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);

    stalled = true;
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(fifo[SVGA_FIFO_BUSY] == 1);
    uint32_t wakes = sync_writes;

    /* The four-register reconfiguration reuses the old word as FIFO data. */
    const uint32_t busy_sentinel = 0xdecafbad;
    fifo[SVGA_FIFO_BUSY] = busy_sentinel;
    assert(svga_configure_locked(false) == 0);
    assert(!host_busy_supported && fifo[SVGA_FIFO_BUSY] == busy_sentinel);
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(sync_writes == wakes + 1 && fifo[SVGA_FIFO_BUSY] == busy_sentinel);
    stalled = false;
    consume();
}

static void prepare_graphical_framebuffer(void);

static void test_trace_capability_survives_fifo_reconfiguration(void)
{
    setup(SVGA_CAP_TRACES | SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    assert(regs[SVGA_REG_TRACES] == 1);

    write_reg(SVGA_REG_CONFIG_DONE, 0);
    svga.fifo_ready = false;
    assert(svga_configure_locked(true) == 0);
    assert(regs[SVGA_REG_TRACES] == 1);

    setup(SVGA_CAP_TRACES, 0, 0);
    assert(svga3d_init() == SVGA_ENOTSUP);
    assert(regs[SVGA_REG_TRACES] == 1);

    setup(0, 0, 0);
    assert(regs[SVGA_REG_TRACES] == 0);

    setup(SVGA_CAP_TRACES, 0, 0);
    prepare_graphical_framebuffer();
    fb.capabilities = FRAMEBUFFER_CAP_MODE_SET;
    fb.max_width = 800;
    fb.max_height = 600;
    fb.max_bytes = 800 * 600 * 4;
    fb.bpp = 32;
    regs[SVGA_REG_TRACES] = 0;
    assert(framebuffer_set_mode(800, 600) == 0);
    assert(regs[SVGA_REG_TRACES] == 1);
    setup(0, 0, 0);
    prepare_graphical_framebuffer();
    fb.capabilities = FRAMEBUFFER_CAP_MODE_SET;
    fb.max_width = 800;
    fb.max_height = 600;
    fb.max_bytes = 800 * 600 * 4;
    fb.bpp = 32;
    assert(framebuffer_set_mode(800, 600) == 0);
    assert(regs[SVGA_REG_TRACES] == 0);
}

static uint32_t legacy_display_fifo[256];

static void prepare_graphical_framebuffer(void)
{
    memset(&fb, 0, sizeof(fb));
    memset(&vmware_svga, 0, sizeof(vmware_svga));
    fb.available = true;
    fb.backend = FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    fb.width = 800;
    fb.height = 600;
    vmware_svga.present = true;
    vmware_svga.fifo_present = true;
    vmware_svga.io_port = 0x456;
    vmware_svga.fifo = legacy_display_fifo;
    vmware_svga.fifo_min = VMWARE_SVGA_FIFO_MIN_BYTES;
    vmware_svga.fifo_max = sizeof(legacy_display_fifo);
    memset(legacy_display_fifo, 0, sizeof(legacy_display_fifo));
    selected_port_reg = 0;
    port_busy_reads = 0;
    port_sync_writes = 0;
}

static void test_display_tick_defers_lock_and_fifo_backpressure(void)
{
    setup(SVGA_CAP_TRACES | SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    prepare_graphical_framebuffer();
    assert(svga.traces_enabled && regs[SVGA_REG_TRACES] == 1);
    uint32_t before_next = fifo[SVGA_FIFO_NEXT_CMD];
    framebuffer_display_tick();
    assert(fifo[SVGA_FIFO_NEXT_CMD] == before_next);

    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    prepare_graphical_framebuffer();
    stalled = true;
    fifo[SVGA_FIFO_NEXT_CMD] = svga.min + 4;
    fifo[SVGA_FIFO_STOP] = svga.min + 8;
    uint32_t before_busy = busy_reads;
    before_next = fifo[SVGA_FIFO_NEXT_CMD];
    framebuffer_display_tick();
    assert(busy_reads - before_busy <= 1);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == before_next);

    stalled = false;
    fifo[SVGA_FIFO_NEXT_CMD] = fifo[SVGA_FIFO_STOP] = svga.min;
    before_next = fifo[SVGA_FIFO_NEXT_CMD];
    uint64_t held = svga_lock();
    framebuffer_display_tick();
    svga_unlock(held);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == before_next);
    framebuffer_display_tick();
    assert(fifo[SVGA_FIFO_NEXT_CMD] != before_next);

    setup(SVGA_CAP_EXTENDED_FIFO, 0, 0);
    prepare_graphical_framebuffer();
    svga.fifo_ready = false;
    vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD] = vmware_svga.fifo_min;
    vmware_svga.fifo[VMWARE_SVGA_FIFO_STOP] = vmware_svga.fifo_min + 4;
    framebuffer_display_tick();
    assert(port_busy_reads == 1);
    vmware_svga.fifo[VMWARE_SVGA_FIFO_STOP] =
        vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD];
    framebuffer_display_tick();
    assert(vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD] !=
           vmware_svga.fifo_min);
}

static void test_fence_last_poll(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    /* Fence waits poll FIFO_FENCE first, then fall back to the bounded
     * BUSY-read loop. A completion caused by the last allowed BUSY read
     * must not be reported as a timeout. */
    host_delay_reads = 1;
    svga_handle fence;
    assert(svga_fence_insert(&fence) == 0);
    assert(svga_fence_wait(fence) == 0);
    assert(!host_delay_reads && fifo[SVGA_FIFO_FENCE] == (uint32_t)fence);
    assert(svga3d_shutdown() == 0);
}
static void test_fallback(void)
{
    setup(SVGA_CAP_EXTENDED_FIFO, 0, 0); assert(!svga.available); assert(!svga_update(0, 0, 10, 10));
    assert(svga3d_init() == SVGA_ENOTSUP && !svga.available && svga.min == 16);
    assert(!svga_update(0, 0, 10, 10));
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR, SVGA_FIFO_CAP_FENCE, 0);
    assert(!svga.available);
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR, SVGA_FIFO_CAP_FENCE, SVGA3D_HWVERSION_WS6_B1);
    assert(!svga.available);
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR, SVGA_FIFO_CAP_FENCE, SVGA3D_HWVERSION_WS8_B1);
    fifo[SVGA_FIFO_3D_CAPS] = 300;
    assert(svga_negotiate_locked() == SVGA_EIO && !svga.available);
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR, SVGA_FIFO_CAP_FENCE, SVGA3D_HWVERSION_WS8_B1);
    fifo[SVGA_FIFO_3D_CAPS] = 0;
    assert(svga_negotiate_locked() == SVGA_ENOTSUP && !svga.available);
    invalid_caps_record = true;
    assert(svga3d_init() == SVGA_EIO && !svga.available && svga.min == 16);
    assert(!svga_update(0, 0, 10, 10));
    struct svga_info info;
    svga_get_info(&info);
    assert(info.probe.status == SVGA_EIO && !strcmp(info.probe.stage, "cap-record"));
    assert(info.fifo_caps == 0 && info.probe.fifo_caps == SVGA_FIFO_CAP_FENCE);
    assert(info.probe.host_legacy == SVGA3D_HWVERSION_WS8_B1);
    assert(info.probe.fifo_min > 16 && info.probe.mem_regs == SVGA_FIFO_NUM_REGS);
    assert(!info.probe.gb_objects && devcap_queries == 0);

    setup(0x1dffc3e2u, 0x77fu, 0);
    assert(svga3d_init() == 0);
    svga_get_info(&info);
    assert(info.available && info.fifo_caps == fifo_caps && info.probe.fifo_caps == fifo_caps);
    assert(!strcmp(info.probe.stage, "ready-gb") && info.probe.gb_objects);
    assert(info.probe.devcap_3d == 1 && devcap_queries >= 2);
    host_devcap_3d = 0;
    assert(svga3d_init() == SVGA_ENOTSUP);
    svga_get_info(&info);
    assert(info.probe.gb_objects && !info.probe.devcap_3d);
    assert(!svga_update(0, 0, 10, 10));

    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    assert(svga3d_init() == SVGA_ENOTSUP);
    svga_get_info(&info);
    assert(!strcmp(info.probe.stage, "fence"));
    assert(info.probe.fifo_caps == SVGA_FIFO_CAP_RESERVE);
    assert(info.probe.host_legacy == SVGA3D_HWVERSION_WS8_B1);
    assert(!svga_update(0, 0, 10, 10));
}
static void test_cache_policy_failure_restores_legacy_fifo(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    uint64_t flags = svga_lock();
    int ret = svga_restore_legacy_after_cache_error_locked(SVGA_ENOTSUP);
    svga_unlock(flags);
    assert(ret == SVGA_ENOTSUP && svga.fifo_ready && !svga.available);
    assert(regs[SVGA_REG_CONFIG_DONE] == 1u);

    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    svga.fifo = NULL;
    flags = svga_lock();
    ret = svga_restore_legacy_after_cache_error_locked(SVGA_ENOTSUP);
    svga_unlock(flags);
    assert(ret == SVGA_EIO);
}
static void test_cache_policy_reports_with_svga_lock_released(void)
{
    assert_console_log_unlocked = true;
    uint32_t previous_count = console_log_count;
    struct svga_cache_report report = {
        .policy = "default-PTE WB+NX requested for validated BAR2",
    };
    svga_report_cache_result_unlocked(&report);
    assert(console_log_count == previous_count + 1);
    assert(strstr(console_log_text, "default-PTE WB+NX requested for validated BAR2"));

    report = (struct svga_cache_report){
        .policy = "UC+NX fallback",
        .reason = "below-user-top",
    };
    svga_report_cache_result_unlocked(&report);
    assert(strstr(console_log_text, "UC+NX fallback"));
    assert(strstr(console_log_text, "below-user-top"));

    report = (struct svga_cache_report){
        .reason = "reservation-mismatch",
        .status = SVGA_ENOTSUP,
        .legacy_restored = true,
    };
    svga_report_cache_result_unlocked(&report);
    assert(strstr(console_log_text, "legacy 2D restored"));

    report.legacy_restored = false;
    svga_report_cache_result_unlocked(&report);
    assert(strstr(console_log_text, "legacy 2D restore failed"));
    assert(console_log_count == previous_count + 4);
    assert_console_log_unlocked = false;
    puts("SVGA cache-policy reports present outside device lock");
}
static void test_effective_surface_limit_snapshot(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR2,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    regs[SVGA_REG_MEMORY_SIZE] = 80u * 1024u * 1024u;
    regs[SVGA_REG_VRAM_SIZE] = 16u * 1024u * 1024u;
    assert(svga3d_init() == 0);
    struct svga_info info;
    svga_get_info(&info);
    assert(info.available &&
           info.surface_limit_bytes == 64u * 1024u * 1024u &&
           info.compat_surface_limit_bytes == 64u * 1024u * 1024u);
    assert(svga3d_shutdown() == 0);

    /* A GB host's non-GB compatibility profile is half the effective MOB
     * page budget, and that budget respects the backend's 64 MiB page cap. */
    setup(0x1dffc3e2u, 0x77fu, 0);
    regs[SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB] = 256u * 1024u;
    assert(svga3d_init() == 0);
    svga_get_info(&info);
    assert(info.available && info.probe.gb_objects &&
           info.surface_limit_bytes == 64u * 1024u * 1024u &&
           info.compat_surface_limit_bytes == 32u * 1024u * 1024u);
    assert(svga3d_shutdown() == 0);
}
static void test_resources(void)
{
    for (uint32_t gmr2 = 0; gmr2 < 3; ++gmr2) {
        setup(gmr2 == 2 ? 0x1dffc3e2u :
              SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | (gmr2 ? SVGA_CAP_GMR2 : SVGA_CAP_GMR),
              SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, gmr2 == 2 ? 0 : SVGA3D_HWVERSION_WS8_B1);
        uint32_t baseline = live_pages;
        struct svga_surface_desc cube_desc = {
            SVGA3D_X8R8G8B8, SVGA3D_SURFACE_CUBEMAP, 32, 32, 1, 3};
        svga_handle cube;
        assert(!svga3d_surface_create(&cube_desc, &cube));
        assert(!svga3d_surface_destroy(cube));
        for (uint32_t i = 0; i < 8; ++i) {
            svga_handle c, s, b;
            struct svga_surface_desc desc = {SVGA3D_X8R8G8B8, SVGA3D_SURFACE_HINT_RENDERTARGET, 64, 64, 1, 1};
            assert(!svga3d_context_create(&c)); assert(!svga3d_surface_create(&desc, &s));
            assert(!svga3d_set_render_target(c, SVGA3D_RT_COLOR0, s));
            assert(svga3d_surface_destroy(s) == SVGA_EBUSY);
            assert(!svga_gmr_create(64 * 64 * 4, &b));
            uint32_t v = 0x12345678, read = 0;
            assert(!svga_gmr_transfer(b, 0, &v, 4, true));
            assert(!svga_gmr_transfer(b, 0, &read, 4, false) && read == v);
            assert(svga_gmr_transfer(b, UINT32_MAX, &v, 4, true) == SVGA_EINVAL);
            struct svga_dma_box box = {0, 0, 0, 64, 64, 1, 0, 0, 0};
            assert(!svga3d_surface_dma(s, 0, 0, b, 0, 256, SVGA3D_WRITE_HOST_VRAM, &box, 1));
            assert(svga3d_surface_dma(s, 0, 0, b, 4, 256, SVGA3D_WRITE_HOST_VRAM, &box, 1) == SVGA_EINVAL);
            box.w = 65;
            assert(svga3d_surface_dma(s, 0, 0, b, 0, 256, SVGA3D_WRITE_HOST_VRAM, &box, 1) == SVGA_EINVAL);
            struct svga_present_rect rect = {0, 0, 64, 64, 0, 0};
            assert(!svga3d_present(s, &rect, 1));
            assert(!svga3d_context_destroy(c)); assert(!svga3d_surface_destroy(s)); assert(!svga_gmr_destroy(b));
            assert(svga_resource_id(s) == UINT32_MAX);
            assert(live_pages == baseline && svga.surface_bytes == 0);
        }
        const uint32_t modes[][2] = {{800, 600}, {641, 479}, {1280, 720},
                                    {2048, 1536}, {1, 1}};
        for (uint32_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
            regs[SVGA_REG_WIDTH] = modes[mode][0];
            regs[SVGA_REG_HEIGHT] = modes[mode][1];
            uint32_t uploads = triangle_uploads;
            assert(!svga3d_triangle_test());
            assert(triangle_uploads == uploads + 1);
            assert(live_pages == baseline && svga.surface_bytes == 0);
        }
        fail_after = allocation_calls;
        svga_handle b; assert(svga_gmr_create(4096, &b) == SVGA_ENOMEM && live_pages == baseline);
        fail_after = UINT32_MAX;
        assert(!svga_gmr_create(4096, &b));
        stalled = true;
        assert(svga_gmr_destroy(b) == SVGA_ETIMEDOUT && live_pages != 0);
        stalled = false; consume();
        assert(!svga_gmr_destroy(b) && live_pages == baseline);
        if (gmr2) {
            /* Force GMR2 DEFINE to fit while its first REMAP cannot fit. The
             * caller must receive ownership until the host can unmap it. */
            stalled = true;
            uint32_t data[4] = {0};
            struct svga_span span = {data, sizeof(data)};
            for (;;) {
                uint32_t next = fifo[SVGA_FIFO_NEXT_CMD], stop = fifo[SVGA_FIFO_STOP];
                uint32_t room = next >= stop ? FIFO_BYTES - next + stop - svga.min : stop - next;
                if (room < 512) break;
                assert(!svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1));
            }
            b = 0;
            assert(svga_gmr_create(512 * 4096u, &b) == SVGA_ETIMEDOUT && b && live_pages == baseline + 512);
            stalled = false;
            consume();
            assert(!svga_gmr_destroy(b) && live_pages == baseline);
        }
        assert(!svga3d_shutdown());
    }
}

static void test_surface_size_rejects_mip_sum_overflow(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    svga.cap_values[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH] = 2000000008u;
    svga.cap_values[SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT] = 1844674400u;
    struct svga_surface_desc desc = {SVGA3D_X8R8G8B8,
        SVGA3D_SURFACE_HINT_RENDERTARGET, 2000000008u, 1844674400u, 1, 2};
    svga_handle surface = 0;
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t pages = live_pages, allocations = allocation_calls;
    uint32_t commands = command_count;
    stalled = true;
    assert(svga3d_surface_create(&desc, &surface) == SVGA_EINVAL);
    assert(!surface && !svga.surface_bytes);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == next && command_count == commands);
    assert(live_pages == pages && allocation_calls == allocations);
    stalled = false;
    consume();
    assert(!svga3d_shutdown() && !live_pages);

    setup(0x1dffc3e2u, 0x77fu, 0);
    svga.cap_values[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH] = 2000000008u;
    svga.cap_values[SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT] = 1844674400u;
    surface = 0;
    next = fifo[SVGA_FIFO_NEXT_CMD];
    pages = live_pages;
    allocations = allocation_calls;
    commands = command_count;
    assert(svga3d_surface_try_create(&desc, &surface) == SVGA_EINVAL);
    assert(!surface && !svga.surface_bytes && !svga.mode_leases);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == next && command_count == commands);
    assert(live_pages == pages && allocation_calls == allocations);
    assert(!svga3d_shutdown() && !live_pages);
}

static struct reliefos_gpu_context gpu_request(void)
{
    return (struct reliefos_gpu_context){
        .size = sizeof(struct reliefos_gpu_context), .version = RELIEFOS_GPU_ABI_VERSION,
        .width = 16, .height = 8, .vertex_capacity = 6};
}
static void test_gpu(void)
{
    struct reliefos_gpu_context request = gpu_request();
    setup(SVGA_CAP_EXTENDED_FIFO, 0, 0);
    assert(svga_gpu_create(17, &request) == SVGA_ENODEV && !request.handle);
    for (uint32_t gmr2 = 0; gmr2 < 3; ++gmr2) {
        setup(gmr2 == 2 ? 0x1dffc3e2u :
              SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | (gmr2 ? SVGA_CAP_GMR2 : SVGA_CAP_GMR),
              SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, gmr2 == 2 ? 0 : SVGA3D_HWVERSION_WS8_B1);
        uint32_t baseline = live_pages;
        request = gpu_request();
        assert(svga_gpu_create(0, &request) == SVGA_EINVAL);
        request.width = 0;
        assert(svga_gpu_create(17, &request) == SVGA_EINVAL);
        request = gpu_request();
        request.version = 99;
        assert(svga_gpu_create(17, &request) == SVGA_EINVAL);
        request = gpu_request();
        assert(!svga_gpu_create(17, &request) && request.handle);
        uint64_t stale = request.handle;
        struct reliefos_gpu_vertex vertices[6] = {
            {-1, -1, 0.5f, 0xffff0000}, {1, -1, 0.5f, 0xff00ff00}, {0, 1, 0.5f, 0xff0000ff},
            {-1, -1, 0.5f, 0xffff0000}, {1, -1, 0.5f, 0xff00ff00}, {0, 1, 0.5f, 0xff0000ff}};
        struct reliefos_gpu_draw draws[2] = {
            {.first = 0, .count = 3, .transform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0.25f,0,0,1}},
            {.first = 3, .count = 3, .transform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0.5f,0,0,1}}};
        uint32_t pixels[129];
        memset(pixels, 0xa5, sizeof(pixels));
        struct reliefos_gpu_frame frame = {
            .size = sizeof(frame), .version = RELIEFOS_GPU_ABI_VERSION, .handle = request.handle,
            .vertices = UINT64_MAX, .draws = UINT64_MAX, .pixels = UINT64_MAX,
            .vertex_count = 6, .draw_count = 2, .pixel_capacity = 128,
            .fill_mode = RELIEFOS_GPU_FILL_SOLID, .clear_color = 0xff324456};
        struct reliefos_gpu_info before, after;
        svga_gpu_get_info(&before);
        assert(before.contexts == 1 && before.surface_bytes && before.guest_bytes);
        assert((before.flags & (RELIEFOS_GPU_AVAILABLE | RELIEFOS_GPU_BUSY_ESTIMATED)) == 3);
        uint32_t commands = command_count;
        assert(svga_gpu_destroy(18, request.handle) == SVGA_EINVAL);
        assert(svga_gpu_render(18, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        frame.pixel_capacity = 127;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        frame.pixel_capacity = 128;
        frame.pixel_capacity = 129;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        frame.pixel_capacity = 128;
        draws[1].first = UINT32_MAX;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        draws[1].first = 3;
        draws[1].count = 2;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        draws[1].count = 3;
        uint32_t nan = 0x7fc00000;
        memcpy(&vertices[0].x, &nan, sizeof(nan));
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        vertices[0].x = -1;
        memcpy(&draws[0].transform[0], &nan, sizeof(nan));
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        draws[0].transform[0] = 1;
        frame.vertex_count = 7;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        frame.vertex_count = 6;
        frame.draw_count = 0;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        frame.draw_count = 2;
        frame.fill_mode = 0;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        frame.fill_mode = RELIEFOS_GPU_FILL_SOLID;
        assert(command_count == commands);
        for (uint32_t i = 0; i < 129; ++i) assert(pixels[i] == 0xa5a5a5a5);
        uint32_t presents = present_count, reads = readback_count, uploads = allocation_calls;
        for (uint32_t fill = 1; fill <= 3; ++fill) {
            frame.fill_mode = fill;
            memset(pixels, 0xcc, sizeof(pixels));
            assert(!svga_gpu_render(17, &frame, vertices, draws, pixels));
            assert(pixels[0] == 0x00112233 && pixels[127] == 0x00aabbcc);
            assert(pixels[64] == 0x00324456 && pixels[128] == 0xcccccccc);
        }
        assert(present_count == presents && readback_count == reads + 3);
        assert(allocation_calls == uploads && depth_draw_count >= 6 && transform_count >= 8);
        svga_gpu_get_info(&after);
        assert(after.submitted_frames == before.submitted_frames + 3);
        assert(after.completed_frames == before.completed_frames + 3);
        assert(after.triangles == before.triangles + 6);
        assert(after.failed_frames == before.failed_frames);
        assert(after.busy_ticks > before.busy_ticks);
        assert(after.busy_ticks - before.busy_ticks <= after.sample_ticks - before.sample_ticks);
        assert(!svga_gpu_destroy(17, request.handle));
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        assert(live_pages == baseline && !svga.surface_bytes);
        for (uint32_t i = 0; i < 24; ++i) {
            request = gpu_request();
            assert(!svga_gpu_create(17, &request) && request.handle != stale);
            svga_gpu_release_owner(17);
            assert(live_pages == baseline && !svga.surface_bytes);
        }
        for (uint32_t fail = 0; fail < (gmr2 == 2 ? 7u : gmr2 ? 2u : 4u); ++fail) {
            request = gpu_request();
            fail_after = allocation_calls + fail;
            assert(svga_gpu_create(17, &request) == SVGA_ENOMEM && !request.handle);
            assert(live_pages == baseline && !svga.surface_bytes);
        }
        fail_after = UINT32_MAX;
        struct reliefos_gpu_context quota[5];
        for (uint32_t i = 0; i < 4; ++i) {
            quota[i] = gpu_request();
            assert(!svga_gpu_create(17, &quota[i]));
        }
        quota[4] = gpu_request();
        assert(svga_gpu_create(17, &quota[4]) == SVGA_ENOMEM && !quota[4].handle);
        svga_gpu_release_owner(17);
        assert(live_pages == baseline && !svga.surface_bytes);
        request = gpu_request();
        stall_command = gmr2 ? SVGA_CMD_DEFINE_GMR2 : SVGA_3D_CMD_SURFACE_DEFINE;
        assert(svga_gpu_create(17, &request) == SVGA_ETIMEDOUT && !request.handle);
        assert(live_pages);
        stalled = false;
        consume();
        svga_gpu_release_owner(17);
        assert(live_pages == baseline && !svga.surface_bytes);
        request = gpu_request();
        assert(!svga_gpu_create(17, &request));
        frame.handle = request.handle;
        stalled = true;
        memset(pixels, 0xa5, sizeof(pixels));
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_ETIMEDOUT);
        for (uint32_t i = 0; i < 129; ++i) assert(pixels[i] == 0xa5a5a5a5);
        struct reliefos_gpu_diagnostics diagnostic;
        uint32_t diagnostic_reads = busy_reads, diagnostic_wakes = sync_writes;
        svga_gpu_get_diagnostics(17, &diagnostic);
        assert(diagnostic.status == -110 && diagnostic.stage == RELIEFOS_GPU_ERROR_FENCE);
        assert(diagnostic.size == sizeof(diagnostic) && diagnostic.version == 1);
        assert(diagnostic.handle == frame.handle && diagnostic.generation == svga.generation);
        assert(diagnostic.fifo_next != diagnostic.fifo_stop);
        assert(diagnostic.fifo_fence != diagnostic.issued_fence);
        assert(diagnostic.issued_fence != svga.issued_fence);
        assert(busy_reads == diagnostic_reads && sync_writes == diagnostic_wakes);
        svga_gpu_get_diagnostics(18, &diagnostic);
        assert(!diagnostic.status && !diagnostic.handle);
        svga_gpu_release_owner(17);
        svga_gpu_get_diagnostics(17, &diagnostic);
        assert(!diagnostic.status);
        assert(live_pages);
        stalled = false;
        consume();
        svga_gpu_get_info(&after);
        assert(live_pages == baseline && !svga.surface_bytes && !after.contexts);
        request = gpu_request();
        assert(!svga_gpu_create(18, &request));
        assert(svga_gpu_destroy(18, frame.handle) == SVGA_EINVAL);
        svga_gpu_get_info(&after);
        assert(after.contexts == 1 && after.failed_frames >= 1);
        svga_gpu_release_owner(18);
        assert(live_pages == baseline && !svga.surface_bytes);
        request = gpu_request();
        assert(!svga_gpu_create(17, &request));
        frame.handle = request.handle;
        svga_gpu_get_info(&before);
        stall_command = SVGA_3D_CMD_DRAW_PRIMITIVES;
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_ETIMEDOUT);
        for (uint32_t i = 0; i < 129; ++i) assert(pixels[i] == 0xa5a5a5a5);
        svga_gpu_get_diagnostics(17, &diagnostic);
        assert(diagnostic.status == -110 && diagnostic.stage == RELIEFOS_GPU_ERROR_FENCE);
        assert(diagnostic.handle == frame.handle && diagnostic.fifo_next != diagnostic.fifo_stop);
        assert(diagnostic.completed_frames == before.completed_frames);
        assert(diagnostic.submitted_frames == before.submitted_frames + 1);
        device_ticks += 1000;
        svga_gpu_get_info(&after);
        assert(after.busy_ticks > before.busy_ticks && live_pages);
        assert(svga_gpu_destroy(18, frame.handle) == SVGA_EINVAL);
        assert(svga_gpu_destroy(17, frame.handle) == SVGA_ETIMEDOUT && live_pages);
        stalled = false;
        consume();
        assert(!svga_gpu_destroy(17, frame.handle));
        svga_gpu_get_info(&before);
        device_ticks += 1000;
        svga_gpu_get_info(&after);
        assert(after.busy_ticks == before.busy_ticks && live_pages == baseline);
        request = gpu_request();
        assert(!svga_gpu_create(17, &request));
        frame.handle = request.handle;
        uint64_t flags;
        assert(!svga_mode_begin(&flags));
        svga_mode_end(flags);
        assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_EINVAL);
        svga_gpu_release_owner(17);
        assert(live_pages == baseline && !svga.surface_bytes);
        svga_gpu_get_info(&before);
        device_ticks = 1;
        svga_gpu_get_info(&after);
        assert(after.generation != before.generation && !after.busy_ticks);
        assert(!svga3d_shutdown());
    }
}

static void test_gpu_diagnostic_skips_unadvertised_fifo_busy(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    regs[SVGA_REG_MEM_REGS] = SVGA_FIFO_BUSY;
    assert(svga_configure_locked(true) == 0 && !svga.fifo_busy_present);
    const uint32_t busy_sentinel = 0xdecafbad;
    fifo[SVGA_FIFO_BUSY] = busy_sentinel;

    struct reliefos_gpu_context request = gpu_request();
    assert(!svga_gpu_create(17, &request));
    struct reliefos_gpu_vertex vertices[6] = {
        {-1, -1, 0.5f, 0xffff0000}, {1, -1, 0.5f, 0xff00ff00},
        {0, 1, 0.5f, 0xff0000ff}, {-1, -1, 0.5f, 0xffff0000},
        {1, -1, 0.5f, 0xff00ff00}, {0, 1, 0.5f, 0xff0000ff},
    };
    struct reliefos_gpu_draw draws[2] = {
        {.first = 0, .count = 3, .transform = {1,0,0,0, 0,1,0,0,
            0,0,1,0, 0.25f,0,0,1}},
        {.first = 3, .count = 3, .transform = {1,0,0,0, 0,1,0,0,
            0,0,1,0, 0.5f,0,0,1}},
    };
    uint32_t pixels[129] = {0};
    struct reliefos_gpu_frame frame = {
        .size = sizeof(frame), .version = RELIEFOS_GPU_ABI_VERSION,
        .handle = request.handle, .vertices = UINT64_MAX, .draws = UINT64_MAX,
        .pixels = UINT64_MAX, .vertex_count = 6, .draw_count = 2,
        .pixel_capacity = 128, .fill_mode = RELIEFOS_GPU_FILL_SOLID,
    };
    stalled = true;
    assert(svga_gpu_render(17, &frame, vertices, draws, pixels) == SVGA_ETIMEDOUT);
    struct reliefos_gpu_diagnostics diagnostic;
    svga_gpu_get_diagnostics(17, &diagnostic);
    assert(diagnostic.status == SVGA_ETIMEDOUT &&
           diagnostic.fifo_busy == UINT32_MAX &&
           fifo[SVGA_FIFO_BUSY] == busy_sentinel);

    stalled = false;
    consume();
    svga_gpu_release_owner(17);
    assert(live_pages == 0 && !svga.surface_bytes);
    assert(!svga3d_shutdown());
}

static void test_gb_recovery(void)
{
    host_render_targets_cap = UINT32_MAX;
    setup(0x1dffc3e2u, 0x77fu, 0);
    assert(svga.gb_active && svga.available && live_pages == svga.pages);
    uint32_t render_targets = 0;
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_RENDER_TARGETS, &render_targets));
    assert(render_targets == SVGA3D_MAX_SIMULTANEOUS_RENDER_TARGETS);
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS,
                              &render_targets));
    assert(render_targets == SVGA3D_MAX_SIMULTANEOUS_RENDER_TARGETS);
    uint32_t baseline = live_pages;
    assert(baseline && svga.host_version == 0x20001u);
    assert(!svga.probe.host_legacy && !svga.probe.host_revised);
    /* No legacy capability records or surface-memory pool are needed. */
    assert(!svga3d_shutdown());
    invalid_caps_record = true;
    regs[SVGA_REG_MEMORY_SIZE] = 0;
    regs[SVGA_REG_ENABLE] = 1;
    regs[SVGA_REG_MEM_REGS] = 291;
    assert(!svga3d_init() && svga.min == 1164 && live_pages == baseline);

    /* Alpha8 is a one-byte texture format. Host operation bits are limited
     * to the VGPU9 behavior implemented by this backend. */
    const uint32_t alpha8_negative_ops = SVGA3DFORMAT_OP_NOFILTER |
        SVGA3DFORMAT_OP_NOALPHABLEND |
        SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP;
    svga.cap_valid[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] = true;
    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_CUBETEXTURE |
        SVGA3DFORMAT_OP_VOLUMETEXTURE |
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
        SVGA3DFORMAT_OP_DISPLAYMODE | SVGA3DFORMAT_OP_SRGBREAD |
        SVGA3DFORMAT_OP_SRGBWRITE | alpha8_negative_ops;
    uint32_t alpha8_bw = 0, alpha8_bh = 0, alpha8_bpp = 0, alpha8_cap = 0;
    assert(!svga_format(SVGA3D_ALPHA8, &alpha8_bw, &alpha8_bh,
                        &alpha8_bpp, &alpha8_cap));
    assert(alpha8_bw == 1 && alpha8_bh == 1 && alpha8_bpp == 1 &&
           alpha8_cap == SVGA3D_DEVCAP_SURFACEFMT_ALPHA8);
    uint32_t alpha8_operations = 0;
    assert(!svga3d_format_operations(SVGA3D_ALPHA8, &alpha8_operations));
    assert(alpha8_operations ==
           (SVGA3DFORMAT_OP_TEXTURE | alpha8_negative_ops));

    struct svga_surface_desc alpha8_texture = {
        SVGA3D_ALPHA8,
        SVGA3D_SURFACE_HINT_TEXTURE |
            SVGA3D_SURFACE_BIND_SHADER_RESOURCE,
        32, 32, 1, 1,
    };
    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
        SVGA3DFORMAT_OP_SRGBWRITE;
    assert(!svga3d_format_operations(SVGA3D_ALPHA8, &alpha8_operations) &&
           !alpha8_operations);
    svga_handle alpha8_rejected = 0;
    assert(svga3d_surface_create(&alpha8_texture, &alpha8_rejected) ==
           SVGA_ENOTSUP);
    assert(!alpha8_rejected && !svga.surface_bytes && live_pages == baseline);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET;
    struct svga_surface_desc alpha8_render_target = alpha8_texture;
    alpha8_render_target.flags = SVGA3D_SURFACE_HINT_RENDERTARGET |
        SVGA3D_SURFACE_BIND_RENDER_TARGET;
    assert(svga3d_surface_create(&alpha8_render_target, &alpha8_rejected) ==
           SVGA_ENOTSUP);
    assert(!alpha8_rejected && !svga.surface_bytes && live_pages == baseline);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        SVGA3DFORMAT_OP_TEXTURE | alpha8_negative_ops;
    uint64_t alpha8_surface_bytes_before = svga.surface_bytes;
    svga_handle alpha8_surface = 0;
    assert(!svga3d_surface_create(&alpha8_texture, &alpha8_surface));
    uint32_t alpha8_sid = svga_resource_id(alpha8_surface);
    assert(svga.surfaces[alpha8_sid].block_bytes == 1u &&
           svga.surfaces[alpha8_sid].bytes == 1024u &&
           svga.surface_bytes == alpha8_surface_bytes_before + 1024u);
    assert(host_mobs[SVGA_CONTEXTS + alpha8_sid].bytes == 1024u);

    svga_handle alpha8_upload = 0, alpha8_download = 0;
    assert(!svga_gmr_create(1024u, &alpha8_upload));
    assert(!svga_gmr_create(1024u, &alpha8_download));
    uint8_t alpha8_source[1024], alpha8_result[1024] = {0};
    for (uint32_t i = 0; i < sizeof(alpha8_source); ++i)
        alpha8_source[i] = (uint8_t)(i * 29u + 7u);
    assert(!svga_gmr_transfer(alpha8_upload, 0, alpha8_source,
                              sizeof(alpha8_source), true));
    struct svga_dma_box alpha8_box = {0, 0, 0, 32, 32, 1, 0, 0, 0};
    uint32_t alpha8_next_before = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t alpha8_commands_before = command_count;
    assert(svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_upload, 0, 31,
        SVGA3D_WRITE_HOST_VRAM, &alpha8_box, 1) == SVGA_EINVAL);
    assert(svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_upload, 1, 32,
        SVGA3D_WRITE_HOST_VRAM, &alpha8_box, 1) == SVGA_EINVAL);
    alpha8_box.x = 32;
    assert(svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_upload, 0, 32,
        SVGA3D_WRITE_HOST_VRAM, &alpha8_box, 1) == SVGA_EINVAL);
    alpha8_box.x = 0;
    assert(fifo[SVGA_FIFO_NEXT_CMD] == alpha8_next_before &&
           command_count == alpha8_commands_before);
    assert(!svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_upload, 0, 32,
        SVGA3D_WRITE_HOST_VRAM, &alpha8_box, 1));
    assert(!svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_download, 0, 32,
        SVGA3D_READ_HOST_VRAM, &alpha8_box, 1));
    assert(!svga_gmr_transfer(alpha8_download, 0, alpha8_result,
                              sizeof(alpha8_result), false));
    assert(!memcmp(alpha8_result, alpha8_source, sizeof(alpha8_source)));
    assert(!svga3d_surface_destroy(alpha8_surface));
    assert(svga.surface_bytes == alpha8_surface_bytes_before);
    assert(!svga_gmr_destroy(alpha8_upload));
    assert(!svga_gmr_destroy(alpha8_download));
    assert(live_pages == baseline);

    /* A nonzero format cap is not enough to use it as a render target. */
    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8] = SVGA3DFORMAT_OP_TEXTURE;
    struct svga_surface_desc unsupported_rt = {SVGA3D_X8R8G8B8,
        SVGA3D_SURFACE_HINT_RENDERTARGET, 64, 64, 1, 1};
    svga_handle rejected_surface = 0;
    assert(svga3d_surface_create(&unsupported_rt, &rejected_surface) == SVGA_ENOTSUP);
    assert(!rejected_surface && live_pages == baseline && !svga.surface_bytes);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8] =
        SVGA3DFORMAT_OP_TEXTURE;
    struct svga_surface_desc bind_only_rt = {SVGA3D_A8R8G8B8,
        SVGA_TEST_SURFACE_BIND_RENDER_TARGET, 64, 64, 1, 1};
    int bind_ret = svga3d_surface_create(&bind_only_rt, &rejected_surface);
    if (!bind_ret) {
        assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
        rejected_surface = 0;
    }
    assert(bind_ret == SVGA_ENOTSUP && !rejected_surface &&
           live_pages == baseline && !svga.surface_bytes);

    for (uint32_t with_vertex_hint = 0; with_vertex_hint < 2; ++with_vertex_hint) {
        struct svga_surface_desc unsupported_stream_output = {
            SVGA3D_BUFFER,
            SVGA_TEST_SURFACE_BIND_STREAM_OUTPUT |
                (with_vertex_hint ? SVGA3D_SURFACE_HINT_VERTEXBUFFER : 0u),
            32768, 1, 1, 1,
        };
        uint32_t allocation_calls_before = allocation_calls;
        bind_ret = svga3d_surface_create(&unsupported_stream_output,
                                         &rejected_surface);
        uint32_t allocation_calls_after = allocation_calls;
        if (!bind_ret) {
            assert(rejected_surface &&
                   !svga3d_surface_destroy(rejected_surface));
            rejected_surface = 0;
        }
        assert(bind_ret == SVGA_ENOTSUP && !rejected_surface &&
               allocation_calls_after == allocation_calls_before &&
               live_pages == baseline && !svga.surface_bytes);
    }

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8] =
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET;
    struct svga_surface_desc bind_only_texture = {SVGA3D_A8R8G8B8,
        SVGA_TEST_SURFACE_BIND_SHADER_RESOURCE, 64, 64, 1, 1};
    bind_ret = svga3d_surface_create(&bind_only_texture, &rejected_surface);
    if (!bind_ret) {
        assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
        rejected_surface = 0;
    }
    assert(bind_ret == SVGA_ENOTSUP && !rejected_surface &&
           live_pages == baseline && !svga.surface_bytes);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8] =
        SVGA3DFORMAT_OP_TEXTURE;
    struct svga_surface_desc bind_only_depth = {SVGA3D_Z_D24S8,
        SVGA_TEST_SURFACE_BIND_DEPTH_STENCIL, 64, 64, 1, 1};
    bind_ret = svga3d_surface_create(&bind_only_depth, &rejected_surface);
    if (!bind_ret) {
        assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
        rejected_surface = 0;
    }
    assert(bind_ret == SVGA_ENOTSUP && !rejected_surface &&
           live_pages == baseline && !svga.surface_bytes);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8] = UINT32_MAX;
    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8] = UINT32_MAX;
    struct svga_surface_desc image_buffer_bind = {SVGA3D_A8R8G8B8,
        SVGA_TEST_SURFACE_BIND_VERTEX_BUFFER, 64, 64, 1, 1};
    bind_ret = svga3d_surface_create(&image_buffer_bind, &rejected_surface);
    if (!bind_ret) {
        assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
        rejected_surface = 0;
    }
    assert(bind_ret == SVGA_ENOTSUP && !rejected_surface &&
           live_pages == baseline && !svga.surface_bytes);

    struct svga_surface_desc buffer_image_bind = {SVGA3D_BUFFER,
        SVGA_TEST_SURFACE_BIND_RENDER_TARGET, 32768, 1, 1, 1};
    bind_ret = svga3d_surface_create(&buffer_image_bind, &rejected_surface);
    if (!bind_ret) {
        assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
        rejected_surface = 0;
    }
    assert(bind_ret == SVGA_ENOTSUP && !rejected_surface &&
           live_pages == baseline && !svga.surface_bytes);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET;
    struct svga_surface_desc bound_color = {SVGA3D_A8R8G8B8,
        SVGA_TEST_SURFACE_BIND_SHADER_RESOURCE |
            SVGA_TEST_SURFACE_BIND_RENDER_TARGET,
        64, 64, 1, 1};
    assert(!svga3d_surface_create(&bound_color, &rejected_surface));
    assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
    rejected_surface = 0;

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL;
    assert(!svga3d_surface_create(&bind_only_depth, &rejected_surface));
    assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
    rejected_surface = 0;

    const uint32_t buffer_bind_flags =
        SVGA_TEST_SURFACE_BIND_VERTEX_BUFFER |
        SVGA_TEST_SURFACE_BIND_INDEX_BUFFER |
        SVGA_TEST_SURFACE_BIND_CONSTANT_BUFFER |
        SVGA_TEST_SURFACE_BIND_SHADER_RESOURCE;
    struct svga_surface_desc bound_buffer = {
        SVGA3D_BUFFER, buffer_bind_flags, 32768, 1, 1, 1,
    };
    assert(!svga3d_surface_create(&bound_buffer, &rejected_surface));
    assert(rejected_surface && !svga3d_surface_destroy(rejected_surface));
    rejected_surface = 0;
    assert(live_pages == baseline && !svga.surface_bytes);

    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8] = UINT32_MAX;
    uint32_t operations = 0;
    assert(!svga3d_format_operations(SVGA3D_X8R8G8B8, &operations));
    assert((operations & (SVGA3DFORMAT_OP_TEXTURE |
                          SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                          SVGA3DFORMAT_OP_DISPLAYMODE |
                          SVGA3DFORMAT_OP_SRGBREAD)) ==
           (SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
            SVGA3DFORMAT_OP_DISPLAYMODE | SVGA3DFORMAT_OP_SRGBREAD));
    assert(!(operations & SVGA3DFORMAT_OP_SRGBWRITE));
    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
        SVGA3DFORMAT_OP_CUBETEXTURE |
        SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
        SVGA3DFORMAT_OP_NOFILTER | SVGA3DFORMAT_OP_NOALPHABLEND |
        SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP;
    assert(!svga3d_format_operations(SVGA3D_A8R8G8B8, &operations));
    assert((operations & SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET) &&
           (operations & SVGA3DFORMAT_OP_NOFILTER) &&
           (operations & SVGA3DFORMAT_OP_NOALPHABLEND) &&
           (operations & SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP) &&
           !(operations & SVGA3DFORMAT_OP_DISPLAYMODE));
    assert(!svga3d_format_operations(SVGA3D_Z_D24S8, &operations));
    assert(operations & SVGA3DFORMAT_OP_ZSTENCIL);
    assert(!svga3d_format_operations(SVGA3D_BUFFER, &operations) && !operations);
    svga_handle surface, context;
    struct svga_surface_desc desc = {SVGA3D_X8R8G8B8, SVGA3D_SURFACE_HINT_RENDERTARGET,
                                    1280, 1024, 1, 1};
    /* A >4 MiB MOB exercises the two-level PPN32 page table. */
    assert(!svga3d_surface_create(&desc, &surface));
    uint32_t sid = svga_resource_id(surface);
    assert(host_mobs[SVGA_CONTEXTS + sid].depth == 2);
    assert(host_mobs[SVGA_CONTEXTS + sid].bytes == 1280 * 1024 * 4u);
    assert(!svga3d_surface_destroy(surface) && live_pages == baseline);
    for (uint32_t fail = 0; fail < 2; ++fail) {
        fail_after = allocation_calls + fail;
        assert(svga3d_surface_create(&desc, &surface) == SVGA_ENOMEM && !surface);
        assert(live_pages == baseline && !svga.surface_bytes);
    }
    fail_after = UINT32_MAX;
    uint64_t saved_phys = next_phys;
    next_phys = (UINT64_C(1) << 44);
    assert(svga3d_context_create(&context) == SVGA_ENOMEM && !context);
    assert(live_pages == baseline);
    next_phys = saved_phys;
    uint32_t page_limit = svga.page_limit;
    svga.page_limit = baseline + 4;
    assert(svga3d_context_create(&context) == SVGA_ENOMEM && !context);
    assert(live_pages == baseline);
    svga.page_limit = page_limit;

    /* Leave room for DEFINE_GB_MOB but not the following object definition. */
    for (uint32_t kind = 0; kind < 2; ++kind) {
        stalled = true;
        uint32_t update[] = {0, 0, 1, 1};
        struct svga_span span = {update, sizeof(update)};
        for (;;) {
            uint32_t next = fifo[SVGA_FIFO_NEXT_CMD], stop = fifo[SVGA_FIFO_STOP];
            uint32_t room = next >= stop ? FIFO_BYTES - next + stop - svga.min : stop - next;
            if (room <= 44) break;
            assert(!svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1));
        }
        svga_handle pending = 0;
        int ret = kind ? svga3d_surface_create(&desc, &pending) : svga3d_context_create(&pending);
        assert(ret == SVGA_ETIMEDOUT && pending && live_pages > baseline);
        stalled = false; consume();
        ret = kind ? svga3d_surface_destroy(pending) : svga3d_context_destroy(pending);
        assert(!ret && live_pages == baseline && !svga.surface_bytes);
    }

    const uint32_t create_stalls[] = {1093, 1107, 1109, 1097, 1099, 1102};
    for (uint32_t i = 0; i < sizeof(create_stalls) / sizeof(create_stalls[0]); ++i) {
        struct reliefos_gpu_context request = gpu_request();
        stall_command = create_stalls[i];
        assert(svga_gpu_create(17, &request) == SVGA_ETIMEDOUT && !request.handle);
        assert(live_pages > baseline);
        stalled = false; consume();
        svga_gpu_release_owner(17);
        assert(live_pages == baseline && !svga.surface_bytes);
    }
    const uint32_t destroy_stalls[] = {1109, 1108, 1094, 1106, 1099, 1098};
    for (uint32_t i = 0; i < sizeof(destroy_stalls) / sizeof(destroy_stalls[0]); ++i) {
        bool is_context = i < 3;
        desc.width = desc.height = 16;
        assert(!svga3d_context_create(&context));
        assert(!svga3d_surface_create(&desc, &surface));
        uint32_t before = live_pages;
        stall_command = destroy_stalls[i];
        int ret = is_context ? svga3d_context_destroy(context) : svga3d_surface_destroy(surface);
        assert(ret == SVGA_ETIMEDOUT && live_pages == before);
        uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
        assert(svga3d_set_render_target(context, SVGA3D_RT_COLOR0, surface) == SVGA_EINVAL);
        uint32_t payload[] = {svga_resource_id(context), 0, 0, 16, 16};
        assert(svga3d_submit(svga.generation, SVGA_3D_CMD_SETVIEWPORT,
                            payload, sizeof(payload)) == SVGA_EBUSY);
        if (!is_context) {
            struct svga_present_rect rect = {0, 0, 16, 16, 0, 0};
            assert(svga3d_present(surface, &rect, 1) == SVGA_EINVAL);
        }
        assert(fifo[SVGA_FIFO_NEXT_CMD] == next);
        stalled = false; consume();
        assert(!svga3d_context_destroy(context));
        assert(!svga3d_surface_destroy(surface));
        assert(live_pages == baseline && !svga.surface_bytes);
    }
    uint32_t shader[] = {0, 0, 0};
    for (uint32_t kind = 0; kind < 2; ++kind) {
        svga_handle vertex;
        struct svga_surface_desc vb = {SVGA3D_BUFFER, SVGA3D_SURFACE_HINT_VERTEXBUFFER,
                                      60, 1, 1, 1};
        assert(!svga3d_context_create(&context));
        assert(!svga3d_surface_create(&desc, &surface));
        assert(!svga3d_surface_create(&vb, &vertex));
        assert(!svga3d_set_render_target(context, SVGA3D_RT_COLOR0, surface));
        assert(!svga3d_draw_triangle(context, surface, vertex, 16, 16));
        stall_command = kind ? 1098 : 1108;
        int ret = kind ? svga3d_surface_destroy(vertex) : svga3d_context_destroy(context);
        assert(ret == SVGA_ETIMEDOUT);
        uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
        assert(svga3d_draw_triangle(context, surface, vertex, 16, 16) == SVGA_EINVAL);
        assert(fifo[SVGA_FIFO_NEXT_CMD] == next);
        stalled = false; consume();
        assert(svga3d_draw_triangle(context, surface, vertex, 16, 16) == SVGA_EINVAL);
        assert(fifo[SVGA_FIFO_NEXT_CMD] == next);
        assert(!svga3d_context_destroy(context));
        assert(!svga3d_surface_destroy(vertex));
        assert(!svga3d_surface_destroy(surface));
        assert(live_pages == baseline);
    }
    assert(svga3d_submit(svga.generation, SVGA_3D_CMD_SHADER_DEFINE,
                        shader, sizeof(shader)) == SVGA_ENOTSUP);
    assert(!svga3d_context_create(&context));
    assert(!svga3d_surface_create(&desc, &surface));
    uint32_t cid = svga_resource_id(context);
    sid = svga_resource_id(surface);
    SVGA3dCmdSetRenderTarget target = {cid, SVGA3D_RT_COLOR0, {sid, 0, 0}};
    assert(!svga3d_submit(svga.generation, SVGA_3D_CMD_SETRENDERTARGET, &target, sizeof(target)));
    assert(svga3d_surface_destroy(surface) == SVGA_EBUSY);
    target.target.sid = UINT32_MAX;
    assert(!svga3d_submit(svga.generation, SVGA_3D_CMD_SETRENDERTARGET, &target, sizeof(target)));
    struct { uint32_t cid; SVGA3dTextureState state; } texture =
        {cid, {.stage = 0, .name = SVGA3D_TS_BIND_TEXTURE, .value = sid}};
    assert(!svga3d_submit(svga.generation, SVGA_3D_CMD_SETTEXTURESTATE, &texture, sizeof(texture)));
    assert(svga3d_surface_destroy(surface) == SVGA_EBUSY);
    texture.state.value = UINT32_MAX;
    assert(!svga3d_submit(svga.generation, SVGA_3D_CMD_SETTEXTURESTATE, &texture, sizeof(texture)));
    assert(!svga3d_surface_destroy(surface));
    assert(!svga3d_context_destroy(context));
    assert(live_pages == baseline);

    stall_command = 1091;
    assert(svga3d_shutdown() == SVGA_ETIMEDOUT && svga.gb_active && !svga.available);
    assert(live_pages == baseline && svga.min == 1164);
    assert(svga_configure_locked(false) == SVGA_EBUSY);
    assert(svga3d_context_create(&context) == SVGA_ENODEV && !context);
    stalled = false; consume();
    assert(!svga3d_shutdown() && !live_pages && svga.min == 16);

    for (uint32_t fail = 0; fail < 6; ++fail) {
        fail_after = allocation_calls + fail;
        assert(svga3d_init() == SVGA_ENOMEM && !live_pages && !svga.gb_active);
        assert(!svga.available && svga.min == 16 && !strcmp(svga.probe.stage, "gb-tables"));
        assert(!svga_update(0, 0, 8, 8));
    }
    fail_after = UINT32_MAX;
    stall_command = 1091;
    assert(svga3d_init() == SVGA_ETIMEDOUT && svga.gb_active && !svga.available);
    assert(live_pages == baseline && svga.min == 1164);
    assert(svga_configure_locked(false) == SVGA_EBUSY);
    uint32_t busy_before = busy_reads;
    assert(svga_update(0, 0, 8, 8) == 0);
    assert(busy_reads == busy_before);
    stalled = false; consume();
    assert(!svga3d_init() && svga.available && live_pages == baseline);
    assert(!svga3d_shutdown() && !live_pages);
    regs[SVGA_REG_MOB_MAX_SIZE] = 4096;
    assert(svga3d_init() == SVGA_ENOTSUP && !svga.gb_active && !live_pages);
    assert(!strcmp(svga.probe.stage, "mob-limits") && !svga_update(0, 0, 8, 8));
    regs[SVGA_REG_MOB_MAX_SIZE] = 128 * 1024 * 1024;
    regs[SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB] = 0;
    assert(svga3d_init() == SVGA_ENOTSUP && !svga.gb_active && !live_pages);
    assert(!svga_update(0, 0, 8, 8));
}
static void test_gb_display_compatibility(void)
{
    setup(SVGA_CAP_EXTENDED_FIFO, 0, 0);
    uint32_t updates = visible_updates;
    assert(!svga_update(0, 0, 16, 16));
    assert(visible_updates == updates + 1);
    capabilities = regs[SVGA_REG_CAPABILITIES] = 0x1dffc3e2u;
    fifo_caps = 0x77fu;
    for (uint32_t cycle = 0; cycle < 2; ++cycle) {
        assert(!svga3d_init() && svga.available);
        updates = visible_updates;
        assert(!svga_update(0, 0, 16, 16));
        assert(visible_updates == updates + 1);
        uint32_t presents = visible_presents;
        assert(!svga3d_triangle_test());
        assert(visible_presents == presents + 1);
        uint64_t flags;
        assert(!svga_mode_begin(&flags));
        svga_mode_end(flags);
        updates = visible_updates;
        assert(svga.available && !svga_update(0, 0, 16, 16));
        assert(visible_updates == updates + 1);
        assert(!svga3d_shutdown() && !live_pages);
        updates = visible_updates;
        assert(!svga_update(0, 0, 16, 16));
        assert(visible_updates == updates + 1);
    }
}

static void test_vgpu9_shader_backend(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    svga_handle first, second;
    assert(!svga3d_context_create(&first));
    assert(!svga3d_context_create(&second));
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu, 0};
    assert(!svga3d_shader_define(first, 7, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    uint32_t global = UINT32_MAX;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        if (svga.shaders[i].defined && svga.shaders[i].context == first &&
            svga.shaders[i].local_id == 7) global = i;
    assert(global < SVGA_GB_SHADER_SLOTS);
    assert(host_mobs[SVGA_SHADER_MOB_BASE + global].bytes == sizeof(code));
    assert(host_mobs[SVGA_SHADER_MOB_BASE + global].depth == 0);
    uint32_t *entry = table_entry(SVGA_OTABLE_SHADER, global,
                                  SVGA_GB_SHADER_ENTRY_BYTES);
    assert(entry[0] == SVGA3D_SHADERTYPE_VS && entry[1] == sizeof(code) &&
           entry[2] == 0 && entry[3] == SVGA_SHADER_MOB_BASE + global);
    assert(svga3d_shader_define(first, 7, SVGA3D_SHADERTYPE_VS,
                                code, sizeof(code)) == SVGA_EINVAL);
    assert(svga3d_shader_define(second, 7, SVGA3D_SHADERTYPE_VS,
                                code, sizeof(code)) == 0);

    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(svga_batch_bind_shader(&batch, first, SVGA3D_SHADERTYPE_VS,
                                  8) == SVGA_EINVAL);
    assert(!svga_batch_bind_shader(&batch, first, SVGA3D_SHADERTYPE_VS, 7));
    uint32_t constants[8] = {0};
    uint32_t before = batch.bytes;
    assert(svga_batch_shader_constants(&batch, first, 255,
             SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT,
             constants, 2) == SVGA_EINVAL);
    assert(batch.bytes == before);
    assert(!svga_batch_shader_constants(&batch, first, 254,
             SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT,
             constants, 2));
    assert(!svga_batch_shader_constants(&batch, first, 15,
             SVGA3D_SHADERTYPE_PS, SVGA3D_CONST_TYPE_INT,
             constants, 1));
    svga_handle fence = 0;
    assert(!svga_batch_submit(&batch, &fence));
    assert(fence && svga.contexts[svga_resource_id(first)].shaders[0] == global);
    bool signaled = false;
    assert(!svga_fence_status(fence, &signaled) && signaled);
    assert(svga3d_shader_destroy(first, 7, SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    assert(!svga3d_shader_destroy(first, 7, SVGA3D_SHADERTYPE_VS));
    assert(!host_mobs[SVGA_SHADER_MOB_BASE + global].bytes);
    assert(svga3d_shader_destroy(second, 7, SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    assert(!svga3d_shader_destroy(second, 7, SVGA3D_SHADERTYPE_VS));
    assert(!svga3d_context_destroy(first));
    assert(!svga3d_context_destroy(second));
    assert(live_pages == baseline);

    assert(!svga3d_context_create(&first));
    uint32_t local = 100;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        assert(!svga3d_shader_define(first, local + i, SVGA3D_SHADERTYPE_PS,
                                     code, 4));
    assert(svga3d_shader_define(first, local + SVGA_GB_SHADER_SLOTS,
                                SVGA3D_SHADERTYPE_PS, code, 4) == SVGA_ENOMEM);
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
        assert(svga3d_shader_destroy(first, local + i,
                                    SVGA3D_SHADERTYPE_PS) == SVGA_EBUSY);
        assert(!svga3d_shader_destroy(first, local + i,
                                      SVGA3D_SHADERTYPE_PS));
    }
    assert(!svga3d_context_destroy(first));
    assert(live_pages == baseline);
    assert(!svga3d_shutdown());
    assert(!live_pages);
}

/* Mesa may switch programs many times in one 64 KiB winsys stream.  This is
 * operation count, not a count of distinct shader resources. */
static void test_many_shader_binds_in_one_batch(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    svga_handle context = 0;
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    assert(!svga3d_context_create(&context));
    assert(!svga3d_shader_define(context, 23, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    for (uint32_t i = 0; i < 41; ++i)
        assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS,
                                       (i & 1u) ? SVGA3D_INVALID_ID : 23));
    svga_handle fence = 0;
    assert(!svga_batch_submit(&batch, &fence));
    bool signaled = false;
    assert(!svga_fence_status(fence, &signaled) && signaled);
    uint32_t cid = svga_resource_id(context);
    assert(host_shaders_bound[cid][0] == svga.shaders[0].host_id);
    assert(svga3d_shader_destroy(context, 23, SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    assert(!svga3d_shader_destroy(context, 23, SVGA3D_SHADERTYPE_VS));
    assert(!svga3d_context_destroy(context));
    assert(live_pages == baseline);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_batch_reference_rollback(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    svga_handle context;
    assert(!svga3d_context_create(&context));
    struct svga_batch batch;
    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  4u, SVGA_BATCH_MAX_INPUT_BYTES));
    uint32_t constants[4] = {0};
    assert(svga_batch_shader_constants(&batch, context, 0,
            SVGA3D_SHADERTYPE_VS, SVGA3D_CONST_TYPE_FLOAT,
            constants, 1) == SVGA_ENOMEM);
    assert(!batch.bytes && !batch.context_count);
    assert(!svga_batch_retain_context(&batch, context));
    assert(batch.context_count == 1 &&
           ((struct svga_batch_metadata *)batch.metadata)->contexts[0] == context);
    assert(!svga_batch_abort(&batch));
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown());
}

static void test_batch_binding_state_is_committed_with_fifo(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0, surface = 0;
    struct svga_surface_desc desc = {SVGA3D_X8R8G8B8,
        SVGA3D_SURFACE_HINT_RENDERTARGET, 16, 16, 1, 1};
    assert(!svga3d_context_try_create(&context));
    assert(!svga3d_surface_try_create(&desc, &surface));
    uint32_t cid = svga_resource_id(context), sid = svga_resource_id(surface);
    assert(svga.contexts[cid].targets[SVGA3D_RT_COLOR0] == SVGA3D_INVALID_ID);

    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    SVGA3dCmdSetRenderTarget target = {
        cid, SVGA3D_RT_COLOR0, {sid, 0, 0},
    };
    assert(!svga_batch_stage_render_target(&batch, context, SVGA3D_RT_COLOR0,
                                           surface, 0, 0));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SETRENDERTARGET, true,
                              &target, sizeof(target)));
    assert(svga.contexts[cid].targets[SVGA3D_RT_COLOR0] == SVGA3D_INVALID_ID);
    uint32_t fifo_next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    svga.fifo[SVGA_FIFO_STOP] = fifo_next + batch.bytes + 8u;
    stalled = true;
    uint32_t commands_before = command_count;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EBUSY);
    assert(!fence && !batch.submitted &&
           svga.contexts[cid].targets[SVGA3D_RT_COLOR0] == SVGA3D_INVALID_ID &&
           svga.fifo[SVGA_FIFO_NEXT_CMD] == fifo_next &&
           command_count == commands_before);
    svga.fifo[SVGA_FIFO_STOP] = fifo_next;
    stalled = false;
    assert(!svga_batch_submit(&batch, &fence));
    assert(svga.contexts[cid].targets[SVGA3D_RT_COLOR0] == sid);
    assert(svga3d_surface_try_destroy(surface) == SVGA_EBUSY);

    struct { uint32_t cid; SVGA3dTextureState state; } texture = {
        cid, {.stage = 3, .name = SVGA3D_TS_BIND_TEXTURE, .value = sid},
    };
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_stage_texture(&batch, context, texture.state.stage,
                                     surface));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SETTEXTURESTATE, true,
                              &texture, sizeof(texture)));
    assert(!svga_batch_submit(&batch, &fence));
    assert(svga.contexts[cid].textures[texture.state.stage] == sid);

    SVGA3dCmdSetRenderTarget unbind = {
        cid, SVGA3D_RT_COLOR0, {SVGA3D_INVALID_ID, 0, 0},
    };
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_stage_render_target(&batch, context, SVGA3D_RT_COLOR0,
                                           0, 0, 0));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SETRENDERTARGET, true,
                              &unbind, sizeof(unbind)));
    assert(!svga_batch_submit(&batch, &fence));
    assert(svga.contexts[cid].targets[SVGA3D_RT_COLOR0] == SVGA3D_INVALID_ID);
    assert(svga.contexts[cid].textures[texture.state.stage] == sid);
    assert(svga3d_surface_try_destroy(surface) == SVGA_EBUSY);

    texture.state.value = SVGA3D_INVALID_ID;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_stage_texture(&batch, context, texture.state.stage, 0));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SETTEXTURESTATE, true,
                              &texture, sizeof(texture)));
    assert(!svga_batch_submit(&batch, &fence));
    assert(svga.contexts[cid].textures[texture.state.stage] == SVGA3D_INVALID_ID);
    consume();
    assert(svga3d_surface_try_destroy(surface) == SVGA_EBUSY);
    consume();
    assert(!svga3d_surface_try_destroy(surface));
    assert(svga3d_context_try_destroy(context) == SVGA_EBUSY);
    consume();
    assert(!svga3d_context_try_destroy(context));
    assert(!svga3d_shutdown());
}

static void test_batch_submit_failure_snapshot_shape_retry_and_fifo(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    struct svga_batch batch, other;
    uint32_t other_storage[SVGA_BATCH_MAX_BYTES / sizeof(uint32_t)];
    uint32_t update[4] = {0, 0, 1, 1};
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update, sizeof(update)));
    struct svga_batch_metadata *metadata = batch.metadata;
    uint32_t old_next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t old_issued = svga.issued_fence;
    uint32_t expected_count = metadata->operation_capacity;
    batch.operation_count = expected_count + 1u;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL);
    assert(!fence && !batch.submitted &&
           svga.fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
        UINT32_MAX, SVGA_BATCH_SUBMIT_DETAIL_OPERATION_COUNT,
        expected_count + 1u, expected_count);

    /* A different batch succeeds without changing the failed batch snapshot. */
    assert(!svga_batch_init(&other, svga.generation, other_storage,
                            sizeof(other_storage)));
    assert(!svga_batch_append(&other, SVGA_CMD_UPDATE, false,
                              update, sizeof(update)));
    assert(!svga_batch_submit(&other, &fence));
    assert_batch_submit_failure(other, SVGA_BATCH_SUBMIT_REASON_NONE,
        UINT32_MAX, 0, 0, 0);
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
        UINT32_MAX, SVGA_BATCH_SUBMIT_DETAIL_OPERATION_COUNT,
        expected_count + 1u, expected_count);

    /* A successful retry clears the old failure on this same caller-owned batch. */
    batch.operation_count = 0;
    assert(!svga_batch_submit(&batch, &fence));
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_NONE,
        UINT32_MAX, 0, 0, 0);

    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update, sizeof(update)));
    old_next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    old_issued = svga.issued_fence;
    uint32_t required = batch.bytes + 8u;
    svga.fifo[SVGA_FIFO_STOP] = old_next + required;
    stalled = true;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EBUSY);
    assert(!batch.submitted && svga.fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_FIFO_TRANSIENT,
        UINT32_MAX, SVGA_BATCH_SUBMIT_DETAIL_FIFO_FREE_BYTES,
        required, required);
    svga.fifo[SVGA_FIFO_STOP] = old_next;
    stalled = false;
    assert(!svga_batch_submit(&batch, &fence));
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_NONE,
        UINT32_MAX, 0, 0, 0);
    assert(!svga3d_shutdown());
}

static void test_batch_submit_failure_snapshot_binding_owner(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0;
    assert(!svga3d_context_create(&context));
    uint32_t cid = svga_resource_id(context);
    struct svga_batch batch;
    uint32_t update[4] = {0, 0, 1, 1};
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_stage_texture(&batch, context, 0, 0));
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update, sizeof(update)));
    uint64_t expected_owner = batch.metadata->transaction_id;
    uint64_t observed_owner = expected_owner + 1u;
    svga.contexts[cid].batch_state_owner = observed_owner;
    uint32_t old_next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t old_issued = svga.issued_fence;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL);
    assert(!fence && !batch.submitted &&
           svga.fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_BINDING,
        0, SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_OWNER,
        expected_owner, observed_owner);
    svga.contexts[cid].batch_state_owner = expected_owner;
    assert(!svga_batch_abort(&batch));
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown());
}

static void test_batch_submit_failure_snapshot_shader_key(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0;
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    assert(!svga3d_context_create(&context));
    assert(!svga3d_shader_define(context, 19, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        if (svga.shaders[i].defined && svga.shaders[i].context == context &&
            svga.shaders[i].local_id == 19)
            slot = i;
    assert(slot < SVGA_GB_SHADER_SLOTS);
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS, 19));
    svga.shaders[slot].local_id = 20;
    uint32_t old_next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t old_issued = svga.issued_fence;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL);
    assert(!fence && !batch.submitted &&
           svga.fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_SHADER,
        0, SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_LOCAL_ID, 19, 20);
    svga.shaders[slot].local_id = 19;
    assert(!svga_batch_abort(&batch));
    consume();
    assert(svga3d_shader_destroy(context, 19, SVGA3D_SHADERTYPE_VS) ==
           SVGA_EBUSY);
    consume();
    assert(!svga3d_shader_destroy(context, 19, SVGA3D_SHADERTYPE_VS));
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown());
}

static void test_batch_submit_failure_snapshot_query_check(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0;
    assert(!svga3d_context_create(&context));
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_query_begin(&batch, context, 0));
    uint32_t observed_kind = UINT32_MAX;
    batch.metadata->query_ops[0].kind = observed_kind;
    (void)observed_kind;
    uint32_t old_next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t old_issued = svga.issued_fence;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL);
    assert(!fence && !batch.submitted &&
           svga.fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    assert_batch_submit_failure(batch, SVGA_BATCH_SUBMIT_REASON_QUERY,
        0, SVGA_BATCH_SUBMIT_DETAIL_QUERY_KIND, observed_kind,
        3u); /* SVGA_QUERY_OP_WAIT is private to query.c. */
    batch.metadata->query_ops[0].kind = 1u;
    assert(!svga_batch_abort(&batch));
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown());
}

static void assert_batch_operation_arrays_unused(const struct svga_batch *batch,
                                                 uint64_t old_bytes,
                                                 uint64_t old_pages)
{
    assert(batch->metadata && !batch->metadata->query_ops &&
           !batch->metadata->shader_ops && !batch->metadata->binding_ops);
    assert(heap_live_bytes - old_bytes == sizeof(*batch->metadata));
    assert(heap_live_pages - old_pages ==
           heap_pages_for_size(sizeof(*batch->metadata)));
    uint64_t eager_bytes = sizeof(*batch->metadata) +
        eager_batch_sidecar_bytes(batch->metadata->operation_capacity);
    uint64_t eager_pages = heap_pages_for_size(sizeof(*batch->metadata)) +
        eager_batch_sidecar_pages(batch->metadata->operation_capacity);
    assert(heap_live_bytes - old_bytes < eager_bytes);
    assert(heap_live_pages - old_pages < eager_pages);
}

static void test_batch_operation_sidecar_demand_cost(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    struct svga_batch batch;
    assert_heap_unlocked = true;
    uint64_t bytes_before = heap_live_bytes, pages_before = heap_live_pages;
    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  64u, 8u));
    assert_batch_operation_arrays_unused(&batch, bytes_before, pages_before);
    uint64_t small_actual_bytes = heap_live_bytes - bytes_before;
    uint64_t small_actual_pages = heap_live_pages - pages_before;
    uint64_t small_eager_bytes = sizeof(*batch.metadata) +
        eager_batch_sidecar_bytes(batch.metadata->operation_capacity);
    uint64_t small_eager_pages = heap_pages_for_size(sizeof(*batch.metadata)) +
        eager_batch_sidecar_pages(batch.metadata->operation_capacity);
    printf("SVGA batch sidecars: small-source allocated %llu bytes/%llu pages; "
           "eager would request %llu bytes/%llu pages\n",
           (unsigned long long)small_actual_bytes,
           (unsigned long long)small_actual_pages,
           (unsigned long long)small_eager_bytes,
           (unsigned long long)small_eager_pages);
    assert(!svga_batch_abort(&batch));

    bytes_before = heap_live_bytes;
    pages_before = heap_live_pages;
    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  64u, 64u));
    struct {
        SVGA3dCmdPresent command;
        SVGA3dCopyRect rect;
    } present = {
        .command = {.sid = 1u},
        .rect = {.x = 2u, .y = 3u, .w = 4u, .h = 5u,
                 .srcx = 2u, .srcy = 3u},
    };
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_PRESENT, true,
                              &present, sizeof(present)));
    assert_batch_operation_arrays_unused(&batch, bytes_before, pages_before);
    assert(!svga_batch_abort(&batch));

    bytes_before = heap_live_bytes;
    pages_before = heap_live_pages;
    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    uint64_t actual_bytes = heap_live_bytes - bytes_before;
    uint64_t actual_pages = heap_live_pages - pages_before;
    uint32_t update[4] = {0, 0, 1, 1};
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update, sizeof(update)));
    assert_batch_operation_arrays_unused(&batch, bytes_before, pages_before);
    uint64_t eager_bytes = sizeof(*batch.metadata) +
        eager_batch_sidecar_bytes(batch.metadata->operation_capacity);
    uint64_t eager_pages = heap_pages_for_size(sizeof(*batch.metadata)) +
        eager_batch_sidecar_pages(batch.metadata->operation_capacity);
    printf("SVGA batch sidecars: max-source allocated %llu bytes/%llu pages; "
           "eager would request %llu bytes/%llu pages\n",
           (unsigned long long)actual_bytes,
           (unsigned long long)actual_pages,
           (unsigned long long)eager_bytes,
           (unsigned long long)eager_pages);
    assert(!svga_batch_abort(&batch));
    assert_heap_unlocked = false;
    assert(!svga3d_shutdown());
}

static void test_batch_operation_sidecar_growth_and_failure(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0;
    assert(!svga3d_context_create(&context));
    struct svga_batch batch;
    assert_heap_unlocked = true;

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  SVGA_BATCH_MAX_OUTPUT_BYTES, 128u));
    uint64_t start_bytes = heap_live_bytes, start_pages = heap_live_pages;
    heap_fail_after = heap_alloc_calls;
    assert(svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS,
                                  SVGA3D_INVALID_ID) == SVGA_ENOMEM);
    heap_fail_after = UINT32_MAX;
    uint32_t cid = svga_resource_id(context);
    assert(!batch.shader_op_count && !batch.operation_count && !batch.bytes &&
           !batch.context_count && !batch.metadata->shader_ops &&
           !svga.contexts[cid].batch_state_owner &&
           heap_live_bytes == start_bytes && heap_live_pages == start_pages);

    for (uint32_t i = 0; i < 4; ++i)
        assert(!svga_batch_bind_shader(&batch, context,
                SVGA3D_SHADERTYPE_VS, SVGA3D_INVALID_ID));
    assert(batch.shader_op_count == 4u && batch.operation_count == 4u);
    assert(heap_allocation_size(batch.metadata->shader_ops) ==
           4u * sizeof(struct svga_batch_shader_op));
    assert(!batch.metadata->query_ops && !batch.metadata->binding_ops);
    struct svga_batch_shader_op *old_shader_ops = batch.metadata->shader_ops;
    assert(!svga_batch_bind_shader(&batch, context,
            SVGA3D_SHADERTYPE_VS, SVGA3D_INVALID_ID));
    assert(batch.shader_op_count == 5u && batch.operation_count == 5u &&
           batch.metadata->shader_ops != old_shader_ops);
    assert(heap_allocation_size(batch.metadata->shader_ops) ==
           8u * sizeof(struct svga_batch_shader_op));
    for (uint32_t i = 5; i < 8; ++i)
        assert(!svga_batch_bind_shader(&batch, context,
                SVGA3D_SHADERTYPE_VS, SVGA3D_INVALID_ID));
    old_shader_ops = batch.metadata->shader_ops;
    uint32_t old_shader_count = batch.shader_op_count;
    uint32_t old_operation_count = batch.operation_count;
    uint32_t old_context_count = batch.context_count;
    uint32_t old_bytes = batch.bytes;
    uint64_t old_transaction = batch.metadata->transaction_id;
    uint64_t old_owner = svga.contexts[cid].batch_state_owner;
    uint64_t old_live_bytes = heap_live_bytes, old_live_pages = heap_live_pages;
    heap_fail_after = heap_alloc_calls;
    assert(svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS,
                                  SVGA3D_INVALID_ID) == SVGA_ENOMEM);
    heap_fail_after = UINT32_MAX;
    assert(batch.metadata->shader_ops == old_shader_ops &&
           heap_allocation_size(batch.metadata->shader_ops) ==
               8u * sizeof(struct svga_batch_shader_op) &&
           batch.shader_op_count == old_shader_count &&
           batch.operation_count == old_operation_count &&
           batch.context_count == old_context_count && batch.bytes == old_bytes &&
           batch.metadata->transaction_id == old_transaction &&
           svga.contexts[cid].batch_state_owner == old_owner &&
           heap_live_bytes == old_live_bytes && heap_live_pages == old_live_pages);
    assert(!svga_batch_abort(&batch));

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  SVGA_BATCH_MAX_OUTPUT_BYTES, 128u));
    assert(!svga_batch_stage_texture(&batch, context, 0, 0));
    assert(batch.binding_op_count == 1u && !batch.metadata->query_ops &&
           !batch.metadata->shader_ops);
    assert(heap_allocation_size(batch.metadata->binding_ops) ==
           4u * sizeof(struct svga_batch_binding_op));
    assert(!svga_batch_abort(&batch));

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  SVGA_BATCH_MAX_OUTPUT_BYTES, 128u));
    assert(!svga_batch_query_begin(&batch, context, 0));
    assert(batch.query_op_count == 1u && !batch.metadata->shader_ops &&
           !batch.metadata->binding_ops);
    assert(heap_allocation_size(batch.metadata->query_ops) ==
           4u * sizeof(struct svga_batch_query_op));
    assert(!svga_batch_abort(&batch));
    assert_heap_unlocked = false;
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown());
}

static void test_batch_operation_capacity_guards(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0;
    assert(!svga3d_context_create(&context));
    struct svga_batch batch;
    assert_heap_unlocked = true;

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  SVGA_BATCH_MAX_OUTPUT_BYTES, 128u));
    assert(!svga_batch_query_begin(&batch, context, 0));
    struct svga_batch_query_op query_storage[4];
    struct svga_batch_query_op *query_ops = batch.metadata->query_ops;
    assert(heap_allocation_size(query_ops) == sizeof(query_storage));
    memcpy(query_storage, query_ops, sizeof(query_storage));
    batch.metadata->query_ops = NULL;
    uint64_t flags = svga_lock();
    assert(svga_query_validate_batch_locked(&batch) == SVGA_EINVAL);
    svga_unlock(flags);
    batch.metadata->query_ops = query_ops;
    batch.query_op_count = 5u;
    batch.operation_count = 5u;
    batch.metadata->query_ops = query_storage;
    flags = svga_lock();
    assert(svga_query_validate_batch_locked(&batch) == SVGA_EINVAL);
    svga_unlock(flags);
    batch.metadata->query_ops = query_ops;
    batch.query_op_count = 1u;
    batch.operation_count = 1u;
    assert(!svga_batch_abort(&batch));

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  SVGA_BATCH_MAX_OUTPUT_BYTES, 128u));
    assert(!svga_batch_stage_texture(&batch, context, 0, 0));
    struct svga_batch_binding_op binding_storage[4];
    struct svga_batch_binding_op *binding_ops = batch.metadata->binding_ops;
    assert(heap_allocation_size(binding_ops) == sizeof(binding_storage));
    memcpy(binding_storage, binding_ops, sizeof(binding_storage));
    batch.metadata->binding_ops = NULL;
    flags = svga_lock();
    assert(svga_batch_state_validate_locked(&batch) == SVGA_EINVAL);
    svga_unlock(flags);
    batch.metadata->binding_ops = binding_ops;
    batch.binding_op_count = 5u;
    batch.operation_count = 5u;
    batch.metadata->binding_ops = binding_storage;
    flags = svga_lock();
    assert(svga_batch_state_validate_locked(&batch) == SVGA_EINVAL);
    svga_unlock(flags);
    batch.metadata->binding_ops = binding_ops;
    batch.binding_op_count = 1u;
    batch.operation_count = 1u;
    assert(!svga_batch_abort(&batch));

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  SVGA_BATCH_MAX_OUTPUT_BYTES, 128u));
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS,
                                   SVGA3D_INVALID_ID));
    struct svga_batch_shader_op shader_storage[4];
    struct svga_batch_shader_op *shader_ops = batch.metadata->shader_ops;
    assert(heap_allocation_size(shader_ops) == sizeof(shader_storage));
    memcpy(shader_storage, shader_ops, sizeof(shader_storage));
    uint32_t shader_count = batch.shader_op_count;
    uint32_t operation_count = batch.operation_count;
    batch.metadata->shader_ops = NULL;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL && !fence);
    batch.metadata->shader_ops = shader_ops;
    batch.shader_op_count = 5u;
    batch.operation_count = 5u;
    batch.metadata->shader_ops = NULL;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL && !fence);
    batch.metadata->shader_ops = shader_storage;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EINVAL && !fence);
    batch.metadata->shader_ops = shader_ops;
    batch.shader_op_count = shader_count;
    batch.operation_count = operation_count;
    assert(!svga_batch_abort(&batch));

    assert_heap_unlocked = false;
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown());
}

static void test_global_batch_metadata_budget(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    struct svga_batch batches[33];
    for (uint32_t i = 0; i < 32; ++i)
        assert(!svga_batch_init_sized(&batches[i], svga.generation,
                batch_storage, 8u, 8u));
    assert(svga_batch_init_sized(&batches[32], svga.generation,
            batch_storage, 8u, 8u) == SVGA_ENOMEM);
    uint64_t mode_flags = 0;
    assert(!svga_mode_begin(&mode_flags));
    svga_mode_end(mode_flags);
    assert(svga_batch_init_sized(&batches[32], svga.generation,
            batch_storage, 8u, 8u) == SVGA_ENOMEM);

    assert(!svga_batch_abort(&batches[0]));
    assert(svga_batch_abort(&batches[0]) == SVGA_EINVAL);
    assert(!svga_batch_init_sized(&batches[32], svga.generation,
            batch_storage, 8u, 8u));
    for (uint32_t i = 1; i < 33; ++i) assert(!svga_batch_abort(&batches[i]));

    /* Large source envelopes hit the byte limit before the independent count
     * ceiling, even though each batch emits only a tiny output packet. */
    uint32_t large_live = 0;
    while (large_live < SVGA_BATCH_METADATA_MAX_LIVE &&
           !svga_batch_init_sized(&batches[large_live], svga.generation,
               batch_storage, 8u, SVGA_BATCH_MAX_INPUT_BYTES))
        ++large_live;
    assert(large_live > 0 && large_live < SVGA_BATCH_METADATA_MAX_LIVE);
    size_t large_metadata_bytes = sizeof(struct svga_batch_metadata) +
        (size_t)(SVGA_BATCH_MAX_INPUT_BYTES / 8u) *
        (sizeof(struct svga_batch_query_op) +
         sizeof(struct svga_batch_shader_op) +
         sizeof(struct svga_batch_binding_op));
    assert((uint64_t)large_live * large_metadata_bytes <=
           SVGA_BATCH_METADATA_MAX_BYTES);
    assert((uint64_t)(large_live + 1u) * large_metadata_bytes >
           SVGA_BATCH_METADATA_MAX_BYTES);
    assert(svga_batch_init_sized(&batches[large_live], svga.generation,
            batch_storage, 8u, SVGA_BATCH_MAX_INPUT_BYTES) == SVGA_ENOMEM);
    for (uint32_t i = 0; i < large_live; ++i)
        assert(!svga_batch_abort(&batches[i]));

    unsigned heap_before = heap_live_allocations;
    heap_fail_after = heap_alloc_calls;
    assert(svga_batch_init_sized(&batches[0], svga.generation,
            batch_storage, 8u, 8u) == SVGA_ENOMEM);
    heap_fail_after = UINT32_MAX;
    assert(heap_live_allocations == heap_before);
    for (uint32_t i = 0; i < 32; ++i)
        assert(!svga_batch_init_sized(&batches[i], svga.generation,
                batch_storage, 8u, 8u));
    assert(svga_batch_init_sized(&batches[32], svga.generation,
            batch_storage, 8u, 8u) == SVGA_ENOMEM);
    for (uint32_t i = 0; i < 32; ++i) assert(!svga_batch_abort(&batches[i]));

    for (uint32_t i = 0; i < 31; ++i)
        assert(!svga_batch_init_sized(&batches[i], svga.generation,
                batch_storage, 8u, 8u));
    assert(!svga_batch_init_sized(&batches[31], svga.generation,
            batch_storage, 20u, 8u));
    const uint32_t update[4] = {0, 0, 1, 1};
    assert(!svga_batch_append(&batches[31], SVGA_CMD_UPDATE, false,
                              &update, sizeof(update)));
    svga_handle fence = 0;
    assert(!svga_batch_submit(&batches[31], &fence) && fence);
    consume();
    assert(!svga_batch_init_sized(&batches[32], svga.generation,
            batch_storage, 8u, 8u));
    for (uint32_t i = 0; i < 31; ++i) assert(!svga_batch_abort(&batches[i]));
    assert(!svga_batch_abort(&batches[32]));
    assert(heap_live_allocations == heap_before);
    assert(!svga3d_shutdown());
}

static uint32_t find_shader_slot(svga_handle context, uint32_t local_id)
{
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        if (svga.shaders[i].defined && svga.shaders[i].context == context &&
            svga.shaders[i].local_id == local_id && !svga.shaders[i].retiring)
            return i;
    return UINT32_MAX;
}

static void test_shader_reaper_rotates_after_budgeted_reap(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    svga_handle first = 0, second = 0, replacement = 0;
    assert(!svga3d_context_create(&first));
    assert(!svga3d_context_create(&second));
    assert(!svga3d_context_create(&replacement));
    assert(!svga3d_shader_define(first, 1, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    assert(!svga3d_shader_define(second, 2, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    assert(!svga.shaders[0].retiring && svga.shaders[0].context == first);
    assert(!svga.shaders[1].retiring && svga.shaders[1].context == second);

    stalled = true;
    assert(svga3d_shader_destroy(first, 1, SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    assert(svga3d_shader_destroy(second, 2, SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    stalled = false;
    consume();
    uint32_t reaped = 0;
    assert(!svga_backend_reap_retired(1, &reaped) && reaped == 1);
    assert(!svga.shaders[0].defined && svga.shaders[1].defined);

    assert(!svga3d_shader_define(replacement, 3, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    assert(svga.shaders[0].defined && svga.shaders[0].context == replacement);
    stalled = true;
    assert(svga3d_shader_destroy(replacement, 3,
            SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    stalled = false;
    consume();
    assert(!svga_backend_reap_retired(1, &reaped) && reaped == 1);
    assert(!svga.shaders[1].defined);
    assert(svga.shaders[0].defined && svga.shaders[0].retiring);
    assert(!svga_backend_reap_retired(1, &reaped) && reaped == 1);
    assert(!svga.shaders[0].defined && !svga.shaders[1].defined);
    assert(!svga3d_context_destroy(first));
    assert(!svga3d_context_destroy(second));
    assert(!svga3d_context_destroy(replacement));
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_context_teardown_reaps_shader_outside_lock(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    svga_handle context = 0;
    assert(!svga3d_context_create(&context));
    assert(!svga3d_shader_define(context, 8, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    uint32_t slot = find_shader_slot(context, 8);
    assert(slot < SVGA_GB_SHADER_SLOTS);
    uint64_t phys = svga.shaders[slot].code.phys;
    assert(phys);
    stalled = true;
    assert(svga3d_shader_destroy(context, 8,
            SVGA3D_SHADERTYPE_VS) == SVGA_EBUSY);
    stalled = false;
    consume();

    unlocked_free_checks = 0;
    assert_unlocked_free_phys = phys;
    assert(!svga3d_context_destroy(context));
    assert(!assert_unlocked_free_phys && unlocked_free_checks == 1);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_shader_batch_transaction_and_retirement(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    svga_handle context = 0;
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    assert(!svga3d_context_create(&context));
    assert(!svga3d_shader_define(context, 7, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    uint32_t old_slot = find_shader_slot(context, 7);
    assert(old_slot < SVGA_GB_SHADER_SLOTS);
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS, 7));
    svga_handle fence = 0;
    assert(!svga_batch_submit(&batch, &fence));
    uint32_t cid = svga_resource_id(context);
    assert(svga.contexts[cid].shaders[0] == old_slot);

    uint32_t base_pages = live_pages, base_accounted = svga.pages;
    uint32_t base_next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t base_issued = svga.issued_fence;
    uint32_t base_commands = command_count;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_shader_destroy(&batch, context, 7,
                                      SVGA3D_SHADERTYPE_VS));
    assert(!svga_batch_shader_define(&batch, context, 7,
            SVGA3D_SHADERTYPE_VS, code, sizeof(code)));
    uint32_t staged_slot = UINT32_MAX;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        if (svga.shaders[i].reserved && svga.shaders[i].context == context)
            staged_slot = i;
    assert(staged_slot < SVGA_GB_SHADER_SLOTS && staged_slot != old_slot);
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS, 7));
    assert(svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS,
                                  0xffffu) == SVGA_EINVAL);
    assert(!svga.shaders[old_slot].retiring &&
           svga.contexts[cid].shaders[0] == old_slot);
    assert(svga.pages == base_accounted && svga.shader_reserved_pages == 1);
    assert(live_pages == base_pages + 1);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == base_next &&
           svga.issued_fence == base_issued && command_count == base_commands);

    uint32_t original_limit = svga.page_limit;
    svga.page_limit = svga.pages + svga.shader_reserved_pages;
    svga_handle denied_gmr = 0;
    assert(svga_gmr_create(4096, &denied_gmr) == SVGA_ENOMEM && !denied_gmr);
    svga.page_limit = original_limit;

    assert(!svga_batch_abort(&batch));
    assert(live_pages == base_pages && svga.pages == base_accounted &&
           !svga.shader_reserved_pages);
    assert(!svga.shaders[old_slot].retiring &&
           svga.contexts[cid].shaders[0] == old_slot &&
           find_shader_slot(context, 7) == old_slot);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == base_next &&
           svga.issued_fence == base_issued && command_count == base_commands);

    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    uint32_t calls_before_failure = allocation_calls;
    fail_after = allocation_calls;
    assert(svga_batch_shader_define(&batch, context, 8,
            SVGA3D_SHADERTYPE_VS, code, sizeof(code)) == SVGA_ENOMEM);
    fail_after = UINT32_MAX;
    assert(allocation_calls == calls_before_failure + 1u);
    assert(!svga_batch_abort(&batch));
    assert(live_pages == base_pages && !svga.shader_reserved_pages);

    assert(!svga_batch_init_sized(&batch, svga.generation, batch_storage,
                                  60u, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(svga_batch_shader_define(&batch, context, 9,
            SVGA3D_SHADERTYPE_VS, code, sizeof(code)) == SVGA_ENOMEM);
    assert(!batch.bytes && svga.pages == base_accounted &&
           live_pages == base_pages && !svga.shader_reserved_pages);
    assert(!svga_batch_abort(&batch));

    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_shader_destroy(&batch, context, 7,
                                      SVGA3D_SHADERTYPE_VS));
    assert(!svga_batch_shader_define(&batch, context, 7,
            SVGA3D_SHADERTYPE_VS, code, sizeof(code)));
    staged_slot = UINT32_MAX;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        if (svga.shaders[i].reserved && svga.shaders[i].context == context)
            staged_slot = i;
    assert(staged_slot < SVGA_GB_SHADER_SLOTS && staged_slot != old_slot);
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS, 7));
    uint32_t original_bytes = batch.bytes;
    uint32_t accepted_bytes = original_bytes + 8u;
    base_next = fifo[SVGA_FIFO_NEXT_CMD];
    base_issued = svga.issued_fence;
    base_commands = command_count;
    fifo[SVGA_FIFO_STOP] = base_next + accepted_bytes;
    stalled = true;
    fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EBUSY);
    assert(!fence && !batch.submitted && batch.bytes == original_bytes);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == base_next &&
           svga.issued_fence == base_issued && command_count == base_commands);
    assert(!svga.shaders[old_slot].retiring &&
           svga.contexts[cid].shaders[0] == old_slot);

    fifo[SVGA_FIFO_STOP] = base_next;
    assert(!svga_batch_submit(&batch, &fence) && fence);
    assert(svga.shaders[old_slot].retiring &&
           svga.shaders[old_slot].retire_fence == fence);
    assert(svga.shaders[staged_slot].defined &&
           !svga.shaders[staged_slot].reserved &&
           svga.contexts[cid].shaders[0] == staged_slot);
    assert(svga.pages == base_accounted + 1u && live_pages == base_pages + 1u);
    uint32_t reaped = UINT32_MAX;
    assert(!svga_backend_reap_retired(1, &reaped) && !reaped);
    assert(live_pages == base_pages + 1u);

    stalled = false;
    consume();
    bool signaled = false;
    assert(!svga_fence_status(fence, &signaled) && signaled);
    assert(!host_shaders[old_slot].defined && host_shaders[staged_slot].defined);
    assert(host_shaders_bound[cid][0] == staged_slot);
    assert(!svga_backend_reap_retired(1, &reaped) && reaped == 1);
    assert(live_pages == base_pages && svga.pages == base_accounted);
    assert(!svga.shaders[old_slot].defined);

    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_shader_destroy(&batch, context, 7,
                                      SVGA3D_SHADERTYPE_VS));
    stalled = true;
    assert(!svga_batch_submit(&batch, &fence));
    assert(svga.shaders[staged_slot].retiring);
    assert(!svga_backend_reap_retired(1, &reaped) && !reaped);
    stalled = false;
    consume();
    assert(!svga_backend_reap_retired(1, &reaped) && reaped == 1);
    assert(live_pages == base_pages - 1u);

    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        assert(!svga3d_shader_define(context, 100u + i,
                SVGA3D_SHADERTYPE_PS, code, sizeof(code)));
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(svga_batch_shader_define(&batch, context, 1000,
            SVGA3D_SHADERTYPE_PS, code, sizeof(code)) == SVGA_ENOMEM);
    assert(!svga.shader_reserved_pages &&
           svga.contexts[cid].batch_state_owner != 0);
    assert(!svga_batch_abort(&batch));
    assert(!svga.contexts[cid].batch_state_owner);
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
        assert(svga3d_shader_destroy(context, 100u + i,
                SVGA3D_SHADERTYPE_PS) == SVGA_EBUSY);
        assert(!svga3d_shader_destroy(context, 100u + i,
                SVGA3D_SHADERTYPE_PS));
    }
    assert(live_pages == base_pages - 1u);
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_sized_batch_and_fence_progress(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    struct svga_batch batch;
    unsigned heap_before = heap_live_allocations;
    heap_fail_after = heap_alloc_calls;
    assert(svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES) == SVGA_ENOMEM);
    heap_fail_after = UINT32_MAX;
    assert(heap_live_allocations == heap_before);
    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(!batch.metadata->query_ops && !batch.metadata->shader_ops &&
           !batch.metadata->binding_ops);
    assert(!svga_batch_abort(&batch));
    assert(heap_live_allocations == heap_before);
    assert(svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES + 4u, SVGA_BATCH_MAX_INPUT_BYTES) == SVGA_EINVAL);
    assert(svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES + 1u) == SVGA_EINVAL);

    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SET_SHADER, true,
            wide_payload, SVGA_BATCH_MAX_OUTPUT_BYTES - 8u));
    assert(batch.bytes == SVGA_BATCH_MAX_OUTPUT_BYTES);
    uint32_t old_next = fifo[SVGA_FIFO_NEXT_CMD], old_issued = svga.issued_fence;
    svga_handle fence = 0;
    assert(svga_batch_submit(&batch, &fence) == SVGA_ENOMEM);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == old_next && svga.issued_fence == old_issued);
    assert(!svga_batch_abort(&batch));

    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(svga_batch_append(&batch, SVGA_3D_CMD_SET_SHADER, true,
            wide_payload, SVGA_BATCH_MAX_OUTPUT_BYTES - 4u) == SVGA_ENOMEM);
    assert(!batch.bytes);
    assert(!svga_batch_abort(&batch));

    memset(wide_fifo, 0, sizeof(wide_fifo));
    svga.fifo = wide_fifo;
    svga.fifo_bytes = SVGA_BATCH_MAX_OUTPUT_BYTES + 8u + 4u + svga.min;
    wide_fifo[SVGA_FIFO_MIN] = svga.min;
    wide_fifo[SVGA_FIFO_MAX] = svga.fifo_bytes;
    wide_fifo[SVGA_FIFO_NEXT_CMD] = wide_fifo[SVGA_FIFO_STOP] = svga.min;
    stalled = true;
    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SET_SHADER, true,
            wide_payload, SVGA_BATCH_MAX_OUTPUT_BYTES - 8u));
    assert(!svga_batch_submit(&batch, &fence));
    struct svga_fence_progress progress;
    assert(!svga_fence_progress_snapshot(&progress));
    assert(progress.generation == svga.generation &&
           progress.issued_seq == (uint32_t)fence &&
           progress.passed_seq != progress.issued_seq);
    bool signaled = true;
    assert(!svga_fence_status(fence, &signaled) && !signaled);
    wide_fifo[SVGA_FIFO_FENCE] = progress.issued_seq;
    assert(!svga_fence_progress_snapshot(&progress));
    assert(progress.passed_seq == progress.issued_seq);
    assert(!svga_fence_status(fence, &signaled) && signaled);

    wide_fifo[SVGA_FIFO_NEXT_CMD] = wide_fifo[SVGA_FIFO_STOP] = svga.min;
    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false, wide_payload, 16));
    old_next = wide_fifo[SVGA_FIFO_NEXT_CMD];
    old_issued = svga.issued_fence;
    uint32_t total = batch.bytes + 8u;
    wide_fifo[SVGA_FIFO_STOP] = old_next + total;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EBUSY);
    assert(wide_fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    wide_fifo[SVGA_FIFO_NEXT_CMD] = wide_fifo[SVGA_FIFO_STOP] = svga.min;
    old_next = wide_fifo[SVGA_FIFO_NEXT_CMD];
    old_issued = svga.issued_fence;
    wide_fifo[SVGA_FIFO_STOP] = old_next + total;
    assert(svga_batch_submit(&batch, &fence) == SVGA_EBUSY);
    assert(wide_fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    wide_fifo[SVGA_FIFO_STOP] = old_next;
    assert(!svga_batch_submit(&batch, &fence));
    wide_fifo[SVGA_FIFO_FENCE] = (uint32_t)fence;
    assert(!svga_fence_progress_snapshot(&progress) &&
           progress.passed_seq == progress.issued_seq);

    svga.fifo_bytes = SVGA_BATCH_MAX_OUTPUT_BYTES + svga.min;
    wide_fifo[SVGA_FIFO_MAX] = svga.fifo_bytes;
    wide_fifo[SVGA_FIFO_NEXT_CMD] = wide_fifo[SVGA_FIFO_STOP] = svga.min;
    old_next = wide_fifo[SVGA_FIFO_NEXT_CMD];
    old_issued = svga.issued_fence;
    assert(!svga_batch_init_sized(&batch, svga.generation, wide_batch_storage,
            SVGA_BATCH_MAX_OUTPUT_BYTES, SVGA_BATCH_MAX_INPUT_BYTES));
    assert(!svga_batch_append(&batch, SVGA_3D_CMD_SET_SHADER, true,
            wide_payload, SVGA_BATCH_MAX_OUTPUT_BYTES - 8u));
    assert(svga_batch_submit(&batch, &fence) == SVGA_ENOMEM);
    assert(wide_fifo[SVGA_FIFO_NEXT_CMD] == old_next &&
           svga.issued_fence == old_issued);
    assert(!svga_batch_abort(&batch));

    /* A body that cannot fit the FIFO is a static size error even when the
     * sequence is also at the transient half-range fence boundary. */
    uint32_t saved_fifo_bytes = svga.fifo_bytes;
    uint32_t saved_next_fence = svga.next_fence;
    uint32_t saved_fifo_fence = wide_fifo[SVGA_FIFO_FENCE];
    uint32_t saved_issued = svga.issued_fence;
    old_next = wide_fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t saved_stop = wide_fifo[SVGA_FIFO_STOP];
    svga_handle saved_fence_token = fence;
    svga.fifo_bytes = svga.min + SVGA_BATCH_MAX_OUTPUT_BYTES + 8u;
    wide_fifo[SVGA_FIFO_MAX] = svga.fifo_bytes;
    wide_fifo[SVGA_FIFO_NEXT_CMD] = wide_fifo[SVGA_FIFO_STOP] = svga.min;
    wide_fifo[SVGA_FIFO_FENCE] = 0;
    svga.next_fence = 0x80000000u;
    fence = UINT64_MAX;
    uint64_t lock_flags = svga_lock();
    assert(svga_fifo_batch_try_locked(wide_payload,
            SVGA_BATCH_MAX_OUTPUT_BYTES, &fence) == SVGA_ENOMEM);
    svga_unlock(lock_flags);
    assert(wide_fifo[SVGA_FIFO_NEXT_CMD] == svga.min &&
           svga.issued_fence == saved_issued && fence == UINT64_MAX);
    svga.fifo_bytes = saved_fifo_bytes;
    wide_fifo[SVGA_FIFO_MAX] = saved_fifo_bytes;
    wide_fifo[SVGA_FIFO_NEXT_CMD] = old_next;
    wide_fifo[SVGA_FIFO_STOP] = saved_stop;
    wide_fifo[SVGA_FIFO_FENCE] = saved_fifo_fence;
    svga.next_fence = saved_next_fence;
    fence = saved_fence_token;

    ++svga.generation;
    assert(!svga_fence_progress_snapshot(&progress));
    assert(progress.generation == svga.generation);
    assert(svga_fence_status(fence, &signaled) == SVGA_EIO);
    --svga.generation;
    svga.issued_fence = (uint32_t)wide_fifo[SVGA_FIFO_FENCE];
    svga.fifo = fifo;
    svga.fifo_bytes = sizeof(fifo);
    fifo[SVGA_FIFO_MIN] = svga.min;
    fifo[SVGA_FIFO_MAX] = sizeof(fifo);
    fifo[SVGA_FIFO_NEXT_CMD] = fifo[SVGA_FIFO_STOP] = svga.min;
    fifo[SVGA_FIFO_FENCE] = svga.issued_fence;
    stalled = false;
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_empty_fence_submission_is_real_and_nonblocking(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    stalled = true;
    uint32_t old_next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t old_issued = svga.issued_fence;
    svga_handle fence = 0;
    int ret = svga_fence_try_insert(&fence);
    assert(!ret && fence);
    assert((uint32_t)(fence >> 32) == svga.generation);
    assert((uint32_t)fence == old_issued + 1u);
    assert(svga.issued_fence == (uint32_t)fence);
    assert(fifo[old_next / 4u] == SVGA_CMD_FENCE);
    assert(fifo[(old_next + 4u) / 4u] == (uint32_t)fence);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == old_next + 8u);
    bool signaled = true;
    assert(!svga_fence_status(fence, &signaled) && !signaled);
    fifo[SVGA_FIFO_FENCE] = (uint32_t)fence;
    assert(!svga_fence_status(fence, &signaled) && signaled);
    stalled = false;
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_old_generation_quiescence_requires_sync_evidence(void)
{
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE,
          SVGA3D_HWVERSION_WS8_B1);
    stalled = true;
    svga_handle fence = 0;
    assert(!svga_fence_try_insert(&fence) && fence);
    assert(!svga_fence_is_quiesced(fence));

    /* A generation transition invalidates ordinary status, but proves no
     * completion of work still held in the previous FIFO. */
    uint32_t submitted_generation = svga.generation;
    ++svga.generation;
    assert(!svga_fence_is_quiesced(fence));
    --svga.generation;
    assert(svga.generation == submitted_generation);
    assert(!svga_fence_is_quiesced(fence));

    /* Only a completed FIFO-wide sync publishes evidence that covers prior
     * sequences in this generation. */
    stalled = false;
    uint64_t flags = svga_lock();
    assert(!svga_sync_locked());
    svga_unlock(flags);
    assert(svga_fence_is_quiesced(fence));

    stalled = true;
    svga_handle later_fence = 0;
    assert(!svga_fence_try_insert(&later_fence) && later_fence);
    assert(!svga_fence_is_quiesced(later_fence));
    stalled = false;
    flags = svga_lock();
    assert(!svga_sync_locked());
    svga_unlock(flags);
    assert(svga_fence_is_quiesced(later_fence));

    stalled = false;
    assert(!svga3d_shutdown() && !live_pages);
}

struct borrowed_test_owner { uint64_t phys; uint32_t pages, puts; };
static void release_borrowed_test_owner(void *opaque)
{
    struct borrowed_test_owner *owner = opaque;
    ++owner->puts;
    deallocate(owner->phys, owner->pages);
}

static void test_borrowed_gmr_mob_lifetime(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    struct borrowed_test_owner owner = {.pages = 300};
    owner.phys = allocate(owner.pages);
    assert(owner.phys);
    svga_handle attachment = 0;
    stall_command = SVGA_CMD_REMAP_GMR2;
    assert(svga_gmr_borrowed_attach(owner.phys, owner.pages * 4096u,
             &owner, release_borrowed_test_owner, &attachment) == SVGA_EBUSY);
    assert(attachment && live_pages == baseline + owner.pages + 1 && owner.puts == 0);
    assert(svga.mode_leases == 1);
    uint64_t mode_flags;
    assert(svga_mode_begin(&mode_flags) == SVGA_EBUSY);
    assert(live_pages == baseline + owner.pages + 1 && owner.puts == 0);
    uint32_t mapped_mob = UINT32_MAX;
    assert(svga_gmr_borrowed_mob_id(attachment, &mapped_mob) == SVGA_EBUSY);
    assert(svga_gmr_borrowed_attach(owner.phys, owner.pages * 4096u,
             &owner, release_borrowed_test_owner, &attachment) == SVGA_EBUSY);
    assert(live_pages == baseline + owner.pages + 1 && owner.puts == 0);
    stalled = false;
    consume();
    int ret;
    do {
        ret = svga_gmr_borrowed_attach(owner.phys, owner.pages * 4096u,
                    &owner, release_borrowed_test_owner, &attachment);
    } while (ret == SVGA_EBUSY && svga.gmrs[svga_resource_id(attachment)].mapped_pages == 0);
    assert(ret == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_attach(owner.phys, owner.pages * 4096u,
             &owner, release_borrowed_test_owner, &attachment));
    assert(!svga_gmr_borrowed_mob_id(attachment, &mapped_mob));
    uint32_t gmr_id = svga_resource_id(attachment);
    assert(mapped_mob == SVGA_EXTERNAL_MOB_BASE + gmr_id);
    assert(host_gmr_pages[gmr_id] == owner.pages);
    assert(host_mobs[mapped_mob].bytes == owner.pages * 4096u);
    for (uint32_t page = 0; page < owner.pages; ++page)
        assert(backing_page(host_mobs[mapped_mob], page) ==
               (uint32_t)(owner.phys >> 12) + page);
    assert(svga_gmr_transfer(attachment, 0, NULL, 0, false) == SVGA_ENOTSUP);

    struct svga_batch batch;
    uint32_t update[] = {0, 0, 1, 1};
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_retain_gmr(&batch, attachment));
    assert(!svga_batch_append(&batch, SVGA_CMD_UPDATE, false,
                              update, sizeof(update)));
    svga_handle fence = 0;
    assert(!svga_batch_submit(&batch, &fence));
    bool signaled = false;
    assert(!svga_fence_status(fence, &signaled) && signaled);

    stall_command = SVGA_CMD_DEFINE_GMR2;
    assert(svga_gmr_borrowed_detach(attachment) == SVGA_EBUSY);
    assert(owner.puts == 0 && host_mobs[mapped_mob].bytes);
    assert(svga_gmr_borrowed_detach(attachment) == SVGA_EBUSY);
    assert(owner.puts == 0 && host_mobs[mapped_mob].bytes);
    stalled = false;
    consume();
    assert(!svga_gmr_borrowed_detach(attachment));
    assert(owner.puts == 1 && live_pages == baseline);
    assert(!svga.mode_leases);
    assert(!host_gmr_pages[gmr_id] && !host_mobs[mapped_mob].bytes);
    assert(!svga_mode_begin(&mode_flags));
    svga_mode_end(mode_flags);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_shutdown_refuses_borrowed_attachment(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    uint32_t svga_pages = svga.pages;
    struct borrowed_test_owner owner = {.pages = 1};
    owner.phys = allocate(owner.pages);
    assert(owner.phys);
    svga_handle attachment = 0;
    assert(svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &attachment) == SVGA_EBUSY);
    assert(attachment && !svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &attachment));
    uint32_t id = svga_resource_id(attachment), mobid = UINT32_MAX;
    assert(id < SVGA_GMRS && !svga_gmr_borrowed_mob_id(attachment, &mobid));
    assert(host_gmr_pages[id] == 1 && host_mobs[mobid].bytes == 4096);
    assert(owner.puts == 0 && live_pages == baseline + 1 && svga.pages == svga_pages);

    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD], stop = fifo[SVGA_FIFO_STOP];
    uint32_t before_busy = busy_reads, before_commands = command_count;
    uint32_t before_sync = sync_writes;
    assert(svga3d_shutdown() == SVGA_EBUSY);
    assert(svga.fifo_ready && svga.available && svga.mode_leases == 1);
    assert(svga.gmrs[id].handle == attachment && svga.gmrs[id].borrowed);
    assert(host_gmr_pages[id] == 1 && host_mobs[mobid].bytes == 4096);
    assert(physical_pointer(owner.phys) && owner.puts == 0);
    assert(live_pages == baseline + 1 && svga.pages == svga_pages);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == next && fifo[SVGA_FIFO_STOP] == stop);
    assert(busy_reads == before_busy && command_count == before_commands &&
           sync_writes == before_sync);

    assert(svga_gmr_borrowed_detach(attachment) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_detach(attachment));
    assert(owner.puts == 1 && live_pages == baseline);
    assert(!host_gmr_pages[id] && !host_mobs[mobid].bytes && !svga.mode_leases);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_vgpu9_occlusion_query_lifetime(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    svga_handle context;
    assert(!svga3d_context_create(&context));
    uint32_t with_context = live_pages;
    struct borrowed_test_owner owner = {.pages = 1};
    owner.phys = allocate(1);
    svga_handle result = 0;
    assert(svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result));
    uint32_t mobid = UINT32_MAX;
    assert(!svga_gmr_borrowed_mob_id(result, &mobid));

    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(svga_batch_query_begin(&batch, context, 4) == SVGA_ENOTSUP);
    assert(!svga_batch_query_begin(&batch, context, 0));
    assert(svga_batch_query_begin(&batch, context, 0) == SVGA_EINVAL);
    assert(svga_batch_query_wait(&batch, context, 0, result, 0) == SVGA_EINVAL);
    assert(svga_batch_query_end(&batch, context, 0, result, 4096 - 8) == SVGA_EINVAL);
    assert(!svga_batch_query_end(&batch, context, 0, result, 0));
    svga_handle end_fence = 0;
    assert(!svga_batch_submit(&batch, &end_fence));
    bool signaled = false;
    assert(!svga_fence_status(end_fence, &signaled) && signaled);
    SVGA3dQueryResult *query = physical_pointer(owner.phys);
    assert(query && query->totalSize == SVGA_GB_QUERY_RESULT_BYTES &&
           query->state == SVGA3D_QUERYSTATE_SUCCEEDED && query->result32 == 5);
    assert(svga3d_context_destroy(context) == SVGA_EBUSY);
    assert(owner.puts == 0 && host_mobs[mobid].bytes);

    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_query_wait(&batch, context, 0, result, 0));
    svga_handle wait_fence = 0;
    assert(!svga_batch_submit(&batch, &wait_fence));
    assert(!svga_fence_status(wait_fence, &signaled) && signaled);
    assert(svga_gmr_borrowed_detach(result) == SVGA_EBUSY);
    assert(!host_mobs[mobid].bytes);
    assert(!svga_gmr_borrowed_detach(result));
    assert(owner.puts == 1 && live_pages == with_context);
    assert(!svga3d_context_destroy(context));
    assert(live_pages == baseline);
    assert(!svga3d_shutdown() && !live_pages);
}

/* A normal stream can contain many interleaved query operations.  Keep
 * operation metadata separate from the deduplicated context/GMR references. */
static void test_many_query_operations_in_one_batch(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    svga_handle context = 0, result = 0;
    assert(!svga3d_context_create(&context));
    struct borrowed_test_owner owner = {.pages = 1};
    owner.phys = allocate(owner.pages);
    assert(owner.phys);
    assert(svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result));
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    for (uint32_t i = 0; i < 128; ++i) {
        uint32_t offset = i * 16u;
        assert(!svga_batch_query_begin(&batch, context, 0));
        assert(!svga_batch_query_end(&batch, context, 0, result, offset));
        assert(!svga_batch_query_wait(&batch, context, 0, result, offset));
    }
    assert(batch.operation_count == 384u && batch.query_op_count == 384u);
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS,
                                   SVGA3D_INVALID_ID));
    for (uint32_t i = batch.operation_count;
         i < SVGA_BATCH_MAX_QUERY_OPS; ++i)
        assert(!svga_batch_stage_texture(&batch, context, 0, 0));
    assert(batch.operation_count == SVGA_BATCH_MAX_QUERY_OPS &&
           batch.query_op_count == 384u && batch.shader_op_count == 1u &&
           batch.binding_op_count == SVGA_BATCH_MAX_QUERY_OPS - 385u);
    assert(heap_allocation_size(batch.metadata->query_ops) ==
           512u * sizeof(struct svga_batch_query_op));
    assert(heap_allocation_size(batch.metadata->shader_ops) ==
           4u * sizeof(struct svga_batch_shader_op));
    assert(heap_allocation_size(batch.metadata->binding_ops) ==
           SVGA_BATCH_MAX_QUERY_OPS * sizeof(struct svga_batch_binding_op));
    /* Binding staging historically reports EINVAL when the shared source
     * operation ceiling is exhausted; keep that API result unchanged. */
    assert(svga_batch_stage_texture(&batch, context, 0, 0) == SVGA_EINVAL);
    svga_handle fence = 0;
    assert(!svga_batch_submit(&batch, &fence));
    bool signaled = false;
    assert(!svga_fence_status(fence, &signaled) && signaled);
    uint32_t cid = svga_resource_id(context);
    assert(!host_queries[cid].active && !host_queries[cid].pending);
    assert(svga_gmr_borrowed_detach(result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_detach(result));
    assert(owner.puts == 1);
    assert(!svga3d_context_destroy(context));
    assert(live_pages == baseline);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_query_pending_capacity_is_bounded(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    svga_handle context = 0, result = 0;
    assert(!svga3d_context_create(&context));
    struct borrowed_test_owner owner = {.pages = 1};
    owner.phys = allocate(owner.pages);
    assert(owner.phys);
    assert(svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result));
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    uint32_t cid = svga_resource_id(context);
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD], issued = svga.issued_fence;
    uint32_t commands = command_count;
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        uint32_t offset = i * 16u;
        assert(!svga_batch_query_begin(&batch, context, 0));
        assert(!svga_batch_query_end(&batch, context, 0, result, offset));
    }
    assert(batch.query_op_count == SVGA_QUERY_PENDING_MAX * 2u);
    assert(!svga_batch_query_begin(&batch, context, 0));
    assert(svga_batch_query_end(&batch, context, 0, result, 0) == SVGA_ENOMEM);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == next && svga.issued_fence == issued &&
           command_count == commands);
    assert(!svga.contexts[cid].query_active && !host_queries[cid].active &&
           !host_queries[cid].pending);
    assert(!svga_batch_abort(&batch));
    assert(!svga.contexts[cid].query_active && !svga.queries[0].used);
    assert(svga_gmr_borrowed_detach(result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_detach(result));
    assert(owner.puts == 1);
    assert(!svga3d_context_destroy(context));
    assert(live_pages == baseline);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_vgpu9_query_retirement_barrier(bool retire_context_first)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    svga_handle context = 0, result = 0;
    assert(!svga3d_context_try_create(&context));
    struct borrowed_test_owner owner = {.pages = 1};
    owner.phys = allocate(1);
    assert(svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_attach(owner.phys, 4096, &owner,
             release_borrowed_test_owner, &result));

    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_query_begin(&batch, context, 0));
    assert(!svga_batch_query_end(&batch, context, 0, result, 0));
    svga_handle end_fence = 0;
    assert(!svga_batch_submit(&batch, &end_fence));
    uint32_t cid = svga_resource_id(context);
    assert(host_queries[cid].pending && owner.puts == 0);

    stalled = true;
    if (retire_context_first)
        assert(svga3d_context_try_destroy(context) == SVGA_EBUSY);
    else
        assert(svga_gmr_borrowed_detach(result) == SVGA_EBUSY);
    uint32_t pending = UINT32_MAX;
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        if (svga.queries[i].used && svga.queries[i].context == cid) {
            pending = i;
            break;
        }
    }
    assert(pending < SVGA_QUERY_PENDING_MAX);
    svga_handle barrier_fence = svga.queries[pending].wait_fence;
    bool barrier_signaled = false;
    assert(barrier_fence && !svga_fence_status(barrier_fence, &barrier_signaled) &&
           !barrier_signaled);
    assert(host_queries[cid].pending && owner.puts == 0);
    assert(svga_gmr_borrowed_detach(result) == SVGA_EBUSY);
    assert(owner.puts == 0 && live_pages > baseline);

    stalled = false;
    consume();
    assert(!host_queries[cid].pending);
    assert(!svga_fence_status(barrier_fence, &barrier_signaled) && barrier_signaled);
    assert(svga3d_context_try_destroy(context) == SVGA_EBUSY);
    assert(!svga3d_context_try_destroy(context));
    assert(!host_contexts[cid]);
    assert(svga_gmr_borrowed_detach(result) == SVGA_EBUSY);
    assert(!svga_gmr_borrowed_detach(result));
    assert(owner.puts == 1 && svga.mode_leases == 0 && live_pages == baseline);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_nonblocking_typed_resource_lifecycle(void)
{
    host_context_cap = SVGA_CONTEXTS;
    host_surface_cap = SVGA_SURFACES;
    host_texture_width_cap = UINT32_MAX;
    host_texture_height_cap = 4096;
    setup(0x1dffc3e2u, 0x77fu, 0);
    uint32_t baseline = live_pages;
    uint32_t before_busy_reads = busy_reads;
    uint32_t value = 0;
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_CONTEXT_IDS, &value));
    assert(value == SVGA_CONTEXTS - 1);
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_SURFACE_IDS, &value));
    assert(value == SVGA_SURFACES - 1);
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH, &value));
    assert(value == svga.surface_limit_bytes / 2u);
    assert(!svga3d_shutdown() && !live_pages);

    host_context_cap = 2;
    host_surface_cap = 3;
    host_texture_width_cap = 2048;
    setup(0x1dffc3e2u, 0x77fu, 0);
    baseline = live_pages;
    assert(svga.context_limit == 2 && svga.surface_limit == 3);
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_CONTEXT_IDS, &value) && value == 1);
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_SURFACE_IDS, &value) && value == 2);
    assert(!svga3d_capability(SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH, &value) && value == 2048);

    /* An atomic create rejected for FIFO pressure owns neither a handle nor MOB pages. */
    stalled = true;
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
    fifo[SVGA_FIFO_STOP] = next + 64;
    svga_handle context = 0, surface = 0;
    struct svga_surface_desc desc = {SVGA3D_X8R8G8B8,
        SVGA3D_SURFACE_HINT_RENDERTARGET, 32, 32, 1, 1};
    assert(svga3d_context_try_create(&context) == SVGA_EBUSY && !context);
    assert(svga3d_surface_try_create(&desc, &surface) == SVGA_EBUSY && !surface);
    assert(live_pages == baseline && !svga.mode_leases);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == next);
    fifo[SVGA_FIFO_STOP] = next;
    stalled = false;
    consume();

    /* The reduced host object limit admits one context and two surfaces. */
    assert(!svga3d_context_try_create(&context) && context);
    svga_handle second = 0;
    assert(svga3d_context_try_create(&second) == SVGA_ENOMEM && !second);
    assert(!svga3d_surface_try_create(&desc, &surface) && surface);
    svga_handle second_surface = 0;
    assert(!svga3d_surface_try_create(&desc, &second_surface) && second_surface);
    assert(svga3d_surface_try_create(&desc, &second) == SVGA_ENOMEM && !second);
    assert(svga.mode_leases == 3);
    uint64_t mode_flags;
    assert(svga_mode_begin(&mode_flags) == SVGA_EBUSY);

    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    assert(!svga3d_shader_define(context, 19, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    uint32_t context_id = svga_resource_id(context), shader_id = UINT32_MAX;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        if (svga.shaders[i].defined && svga.shaders[i].context == context)
            shader_id = i;
    assert(shader_id < SVGA_GB_SHADER_SLOTS);
    struct svga_batch batch;
    assert(!svga_batch_init(&batch, svga.generation, batch_storage,
                            SVGA_BATCH_MAX_BYTES));
    assert(!svga_batch_bind_shader(&batch, context, SVGA3D_SHADERTYPE_VS, 19));
    svga_handle use_fence = 0;
    assert(!svga_batch_submit(&batch, &use_fence));

    /* A stalled host keeps the accepted retirement and all backing alive. */
    uint32_t pages_before_retire = live_pages;
    stalled = true;
    next = fifo[SVGA_FIFO_NEXT_CMD];
    assert(svga3d_surface_try_destroy(surface) == SVGA_EBUSY);
    assert(svga.surfaces[svga_resource_id(surface)].retiring);
    uint32_t accepted_next = fifo[SVGA_FIFO_NEXT_CMD];
    assert(accepted_next != next && live_pages == pages_before_retire);
    assert(svga3d_surface_try_destroy(surface) == SVGA_EBUSY);
    assert(fifo[SVGA_FIFO_NEXT_CMD] == accepted_next);
    stalled = false;
    consume();
    assert(!svga3d_surface_try_destroy(surface));
    assert(svga.mode_leases == 2);

    /* Context teardown must unbind and retire its shader behind the same fence. */
    stalled = true;
    pages_before_retire = live_pages;
    assert(svga3d_context_try_destroy(context) == SVGA_EBUSY);
    assert(live_pages == pages_before_retire);
    assert(svga3d_context_try_destroy(context) == SVGA_EBUSY);
    stalled = false;
    consume();
    assert(!svga3d_context_try_destroy(context));
    assert(svga.mode_leases == 1);
    assert(!host_contexts[context_id] && !host_shaders[shader_id].defined);
    assert(!host_mobs[SVGA_SHADER_MOB_BASE + shader_id].bytes);
    assert(svga3d_surface_try_destroy(second_surface) == SVGA_EBUSY);
    assert(!svga3d_surface_try_destroy(second_surface));
    assert(!svga.mode_leases);
    assert(live_pages == baseline);
    assert(busy_reads == before_busy_reads);
    assert(!svga_mode_begin(&mode_flags));
    svga_mode_end(mode_flags);
    assert(!svga3d_shutdown() && !live_pages);
}

static void test_nonblocking_legacy_resource_lifecycle(void)
{
    host_context_cap = SVGA_CONTEXTS;
    host_surface_cap = SVGA_SURFACES;
    host_texture_width_cap = 4096;
    host_texture_height_cap = 4096;
    setup(SVGA_CAP_3D | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_GMR,
          SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_RESERVE, SVGA3D_HWVERSION_WS8_B1);
    assert(!svga.gb_active && svga.available);
    uint32_t baseline = live_pages;
    svga.cap_valid[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] = true;
    svga.cap_values[SVGA3D_DEVCAP_SURFACEFMT_ALPHA8] =
        SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_NOFILTER |
        SVGA3DFORMAT_OP_NOALPHABLEND |
        SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP;
    struct svga_surface_desc alpha8_desc = {
        SVGA3D_ALPHA8,
        SVGA3D_SURFACE_HINT_TEXTURE |
            SVGA3D_SURFACE_BIND_SHADER_RESOURCE,
        32, 32, 1, 1,
    };
    uint64_t surface_bytes_before = svga.surface_bytes;
    svga_handle alpha8_surface = 0;
    assert(!svga3d_surface_create(&alpha8_desc, &alpha8_surface));
    uint32_t alpha8_sid = svga_resource_id(alpha8_surface);
    assert(svga.surfaces[alpha8_sid].block_bytes == 1u &&
           svga.surfaces[alpha8_sid].bytes == 1024u &&
           svga.surface_bytes == surface_bytes_before + 1024u);
    assert(host_surfaces[alpha8_sid].format == SVGA3D_ALPHA8 &&
           host_surfaces[alpha8_sid].width == 32u &&
           host_surfaces[alpha8_sid].height == 32u &&
           host_surfaces[alpha8_sid].data &&
           !svga.surfaces[alpha8_sid].gb.defined);

    svga_handle alpha8_upload = 0, alpha8_download = 0;
    assert(!svga_gmr_create(1024u, &alpha8_upload));
    assert(!svga_gmr_create(1024u, &alpha8_download));
    uint8_t alpha8_source[1024], alpha8_result[1024] = {0};
    for (uint32_t i = 0; i < sizeof(alpha8_source); ++i)
        alpha8_source[i] = (uint8_t)(i * 17u + 3u);
    assert(!svga_gmr_transfer(alpha8_upload, 0, alpha8_source,
                              sizeof(alpha8_source), true));
    struct svga_dma_box alpha8_box = {0, 0, 0, 32, 32, 1, 0, 0, 0};
    assert(!svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_upload, 0, 32,
        SVGA3D_WRITE_HOST_VRAM, &alpha8_box, 1));
    assert(!svga3d_surface_dma(alpha8_surface, 0, 0, alpha8_download, 0, 32,
        SVGA3D_READ_HOST_VRAM, &alpha8_box, 1));
    assert(!svga_gmr_transfer(alpha8_download, 0, alpha8_result,
                              sizeof(alpha8_result), false));
    assert(!memcmp(alpha8_result, alpha8_source, sizeof(alpha8_source)));
    assert(!svga3d_surface_destroy(alpha8_surface));
    assert(svga.surface_bytes == surface_bytes_before);
    assert(!svga_gmr_destroy(alpha8_upload));
    assert(!svga_gmr_destroy(alpha8_download));
    assert(live_pages == baseline);

    uint32_t before_busy_reads = busy_reads;
    svga_handle context = 0, surface = 0;
    struct svga_surface_desc desc = {SVGA3D_X8R8G8B8,
        SVGA3D_SURFACE_HINT_RENDERTARGET, 64, 64, 1, 1};
    assert(!svga3d_context_try_create(&context) && context);
    assert(!svga3d_surface_try_create(&desc, &surface) && surface);
    assert(svga.mode_leases == 2);
    uint64_t mode_flags;
    assert(svga_mode_begin(&mode_flags) == SVGA_EBUSY);
    assert(svga3d_surface_try_destroy(surface) == SVGA_EBUSY);
    assert(!svga3d_surface_try_destroy(surface));
    assert(svga3d_context_try_destroy(context) == SVGA_EBUSY);
    assert(!svga3d_context_try_destroy(context));
    assert(!svga.mode_leases);
    assert(busy_reads == before_busy_reads);
    assert(!svga3d_shutdown() && !live_pages);
}

/* An idle reaper must not contend with an unrelated GPU batch. Live shaders
 * alone do not imply retirement work; only a submitted destroy does. */
static void test_idle_shader_retirement(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    const uint32_t code[] = {0xfffe0200u, 0x0000ffffu};
    svga_handle context = 0;
    assert(!svga3d_context_create(&context));
    assert(!svga3d_shader_define(context, 1, SVGA3D_SHADERTYPE_VS,
                                 code, sizeof(code)));
    uint64_t flags;
    assert(svga_try_lock_irqsave(&flags));
    uint32_t reaped = 99;
    int result = svga_backend_reap_retired(1, &reaped);
    svga_unlock(flags);
    assert(result == 0 && reaped == 0 &&
           "idle shader reaper contended with unrelated GPU work");
    assert(!svga3d_context_destroy(context));
    assert(!svga3d_shutdown() && !live_pages);
}

int main(void)
{
    test_batch_operation_sidecar_demand_cost();
    test_batch_operation_sidecar_growth_and_failure();
    test_batch_operation_capacity_guards();
    test_batch_submit_failure_snapshot_shape_retry_and_fifo();
    test_batch_submit_failure_snapshot_binding_owner();
    test_batch_submit_failure_snapshot_shader_key();
    test_batch_submit_failure_snapshot_query_check();
    test_global_batch_metadata_budget();
    test_shader_reaper_rotates_after_budgeted_reap();
    test_context_teardown_reaps_shader_outside_lock();
    test_display_tick_defers_lock_and_fifo_backpressure();
    test_trace_capability_survives_fifo_reconfiguration();
    test_gb_display_compatibility();
    test_fence_last_poll(); test_fifo_busy_requires_reported_register();
    test_fifo_busy_state_resets_on_reconfigure(); test_fifo_wakeup();
    test_fifo(); test_nonblocking_batch();
    test_mode_lease_preserves_live_backend(); test_fallback();
    test_cache_policy_failure_restores_legacy_fifo();
    test_cache_policy_reports_with_svga_lock_released();
    test_effective_surface_limit_snapshot();
    test_resources(); test_surface_size_rejects_mip_sum_overflow();
    test_gpu(); test_gpu_diagnostic_skips_unadvertised_fifo_busy();
    test_gb_recovery();
    test_vgpu9_shader_backend();
    test_many_shader_binds_in_one_batch();
    test_batch_reference_rollback();
    test_batch_binding_state_is_committed_with_fifo();
    test_shader_batch_transaction_and_retirement();
    test_sized_batch_and_fence_progress();
    test_empty_fence_submission_is_real_and_nonblocking();
    test_old_generation_quiescence_requires_sync_evidence();
    test_borrowed_gmr_mob_lifetime();
    test_shutdown_refuses_borrowed_attachment();
    test_vgpu9_occlusion_query_lifetime();
    test_many_query_operations_in_one_batch();
    test_query_pending_capacity_is_bounded();
    test_vgpu9_query_retirement_barrier(true);
    test_vgpu9_query_retirement_barrier(false);
    test_nonblocking_typed_resource_lifecycle();
    test_nonblocking_legacy_resource_lifecycle();
    assert(draw_count >= 2);
    test_idle_shader_retirement();
    printf("SVGA tests passed: %u commands, %u presents; no live DMA pages\n", command_count, present_count);
    return 0;
}
