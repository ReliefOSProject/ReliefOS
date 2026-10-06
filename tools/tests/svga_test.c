#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "device.h"

#define FIFO_BYTES 65536u
static uint32_t fifo[FIFO_BYTES / 4], regs[64];
static uint32_t capabilities, host_version, fifo_caps, selected_gmr;
static uint32_t host_devcap_3d, devcap_queries;
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
struct host_surface { uint32_t width, height, format, mobid; uint8_t *data; bool gb; };
static struct host_surface host_surfaces[SVGA_SURFACES];
static bool stalled, invalid_caps_record;
struct allocation { uint64_t phys; void *ptr; uint32_t pages; };
static struct allocation allocations[512];
static uint64_t next_phys = 0x10000000;
static uint32_t live_pages, fail_after = UINT32_MAX, allocation_calls;
static uint32_t host_gmr_pages[SVGA_GMRS], host_gmr_ppn[SVGA_GMRS][SVGA_MAX_GUEST_PAGES];

static void *physical_pointer(uint64_t phys)
{
    device_ticks += 10000;
    for (uint32_t i = 0; i < 512; ++i)
        if (allocations[i].ptr && phys >= allocations[i].phys &&
            phys - allocations[i].phys < allocations[i].pages * 4096ULL)
            return (uint8_t *)allocations[i].ptr + phys - allocations[i].phys;
    return NULL;
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
        host_contexts[object] = false;
        memset(table_entry(2, object, 8), 0, 8);
        break;
    case 1097: {
        assert(size == 36 && object < SVGA_SURFACES && !host_surfaces[object].data);
        struct host_surface *s = &host_surfaces[object];
        s->format = peek(stop, 4); s->width = peek(stop, 8); s->height = peek(stop, 9);
        assert(peek(stop, 5) && !peek(stop, 6) && !peek(stop, 7) && peek(stop, 10));
        s->data = calloc(s->width * s->height, 4);
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
        if (id >= 1091 && id <= 1114) {
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
                if (dma.transfer == SVGA3D_READ_HOST_VRAM) {
                    struct host_surface *surface = &host_surfaces[dma.host.sid];
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
                surface->data = calloc(surface->width * surface->height, 4);
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
    if (fifo[SVGA_FIFO_MIN] > SVGA_FIFO_BUSY * 4u)
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
        case SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH:
        case SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT: return 4096;
        case SVGA3D_DEVCAP_MAX_VOLUME_EXTENT: return 256;
        case SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8:
        case SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8:
        case SVGA3D_DEVCAP_SURFACEFMT_Z_D16:
        case SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8: return UINT32_MAX;
        default: return 0;
        }
    }
    return regs[reg];
}
static void write_reg(uint32_t reg, uint32_t value)
{
    regs[reg] = value;
    if (reg == SVGA_REG_CONFIG_DONE && value) {
        if (fifo[SVGA_FIFO_MIN] > SVGA_FIFO_GUEST_3D_HWVERSION * 4) {
            configured_guest = fifo[SVGA_FIFO_GUEST_3D_HWVERSION];
            fifo[SVGA_FIFO_CAPABILITIES] = fifo_caps;
            fifo[SVGA_FIFO_3D_HWVERSION] = host_version;
            fifo[SVGA_FIFO_3D_HWVERSION_REVISED] = host_version;
            uint32_t p = SVGA_FIFO_3D_CAPS;
            const uint32_t caps[] = {SVGA3D_DEVCAP_3D, 1,
                SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH, 4096, SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT, 4096,
                SVGA3D_DEVCAP_MAX_VOLUME_EXTENT, 256,
                SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8, 0xffffffff,
                SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8, 0xffffffff,
                SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8, 0xffffffff,
                SVGA3D_DEVCAP_SURFACEFMT_Z_D16, 0xffffffff,
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
        if (fifo[SVGA_FIFO_MIN] > SVGA_FIFO_BUSY * 4u) fifo[SVGA_FIFO_BUSY] = 1;
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
    memset(&svga, 0, sizeof(svga)); memset(fifo, 0, sizeof(fifo)); memset(regs, 0, sizeof(regs));
    memset(host_gmr_pages, 0, sizeof(host_gmr_pages));
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
    invalid_caps_record = false;
    host_devcap_3d = 1;
    devcap_queries = 0;
    host_delay_reads = 0;
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
    stalled = true;
    svga_handle f; assert(!svga_fence_insert(&f));
    assert(svga_fence_wait(f) == SVGA_ETIMEDOUT);
    stalled = false; consume();
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
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    assert(svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1) == 0);
    svga_handle fence;
    assert(svga_fence_insert(&fence) == 0);
    /* Conservative submission explicitly notifies the host for every packet. */
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
static void test_gb_recovery(void)
{
    setup(0x1dffc3e2u, 0x77fu, 0);
    assert(svga.gb_active && svga.available && live_pages == svga.pages);
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
int main(void)
{
    test_gb_display_compatibility();
    test_fence_last_poll(); test_fifo_wakeup(); test_fifo(); test_fallback();
    test_resources(); test_gpu(); test_gb_recovery();
    assert(draw_count >= 2);
    printf("SVGA tests passed: %u commands, %u presents; no live DMA pages\n", command_count, present_count);
    return 0;
}
