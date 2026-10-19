#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <reliefnt/port.h>
#include <reliefnt/svga.h>
#include "../../kernel/reliefnt/drivers/video/framebuffer/framebuffer.c"

struct svga_device svga;

static uint32_t gfb[16u * 16u];
static uint32_t fifo_words[16];
static uint32_t doorbells;

uint64_t svga_lock(void) { return 0; }
bool svga_try_lock_irqsave(uint64_t *flags)
{
    if (flags) *flags = 0;
    return true;
}
void svga_unlock(uint64_t flags) { (void)flags; }
int svga_update_try_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    (void)x; (void)y; (void)width; (void)height;
    return 0;
}
int svga_update_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    (void)x; (void)y; (void)width; (void)height;
    return 0;
}
bool svga_restore_traces_locked(void) { return false; }
void x86_64_outl(uint32_t value, uint16_t port)
{
    (void)value; (void)port;
    ++doorbells;
}
uint32_t x86_64_inl(uint16_t port) { (void)port; return 0; }

static void setup_fifo_full(void)
{
    memset(&fb, 0, sizeof(fb));
    fb.pixels = gfb;
    fb.width = 16;
    fb.height = 16;
    fb.pitch = 16u * 4u;
    fb.max_width = 16;
    fb.max_height = 16;
    fb.max_bytes = sizeof(gfb);
    fb.backend = FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    fb.bpp = 32;
    fb.bytes_per_pixel = 4;
    fb.available = true;
    memset(gfb, 0, sizeof(gfb));
    memset(fifo_words, 0, sizeof(fifo_words));
    memset(&vmware_svga, 0, sizeof(vmware_svga));
    memset(&svga, 0, sizeof(svga));
    vmware_svga.present = true;
    vmware_svga.fifo_present = true;
    vmware_svga.io_port = 0x456;
    vmware_svga.fifo = fifo_words;
    vmware_svga.fifo_min = 16;
    vmware_svga.fifo_max = 64;
    fifo_words[VMWARE_SVGA_FIFO_NEXT_CMD] = 40;
    fifo_words[VMWARE_SVGA_FIFO_STOP] = 60;
    framebuffer_drm_scanout_owned = false;
    framebuffer_drm_damage_pending = false;
    doorbells = 0;
}

static void test_full_fifo_damage_is_retained_and_retried(void)
{
    setup_fifo_full();
    assert(framebuffer_drm_scanout_begin() == 0);
    uint32_t source[16u * 16u];
    for (uint32_t i = 0; i < 16u * 16u; ++i) source[i] = 0xff000000u | i;
    assert(framebuffer_drm_present_region(source, 16u * 4u, 0x34325258u,
                                          1, 2, 3, 4) == 0);
    assert(framebuffer_drm_damage_pending);
    assert(gfb[2u * 16u + 1u] == source[2u * 16u + 1u]);
    assert(gfb[3u * 16u + 3u] == 0);

    source[10u * 16u + 8u] = 0xffabcdefu;
    assert(framebuffer_drm_present_region(source, 16u * 4u, 0x34325258u,
                                          8, 10, 9, 11) == 0);
    assert(framebuffer_drm_damage_pending);
    assert(framebuffer_drm_pending_x1 == 1 && framebuffer_drm_pending_y1 == 2 &&
           framebuffer_drm_pending_x2 == 9 && framebuffer_drm_pending_y2 == 11);
    assert(gfb[10u * 16u + 8u] == 0xffabcdefu);

    fifo_words[VMWARE_SVGA_FIFO_NEXT_CMD] = 40;
    fifo_words[VMWARE_SVGA_FIFO_STOP] = 40;
    framebuffer_display_tick();
    assert(!framebuffer_drm_damage_pending);
    assert(fifo_words[VMWARE_SVGA_FIFO_NEXT_CMD] == 60);
    assert(fifo_words[10] == VMWARE_SVGA_CMD_UPDATE);
    assert(fifo_words[11] == 1 && fifo_words[12] == 2 &&
           fifo_words[13] == 8 && fifo_words[14] == 9);
    assert(doorbells > 0);

    framebuffer_drm_scanout_release();
    assert(!framebuffer_drm_damage_pending && !framebuffer_drm_scanout_owned);
    framebuffer_display_tick();
    assert(fifo_words[4] == VMWARE_SVGA_CMD_UPDATE);
    assert(fifo_words[5] == 0 && fifo_words[6] == 0 &&
           fifo_words[7] == 16 && fifo_words[8] == 16);
}

int main(void)
{
    test_full_fifo_damage_is_retained_and_retried();
    puts("framebuffer-drm: PASS");
    return 0;
}
