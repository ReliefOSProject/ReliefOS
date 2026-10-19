#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/fb.h>
#include <reliefnt/framebuffer.h>
#include "../../kernel/reliefnt/kernel/reliefnt/syscall.c"

_Static_assert(FB_ACTIVATE_NOW == 0 && FB_ACTIVATE_NXTOPEN == 1 &&
                   FB_ACTIVATE_TEST == 2 && FB_ACTIVATE_MASK == 15 &&
                   FB_ACTIVATE_VBL == 16 && FB_ACTIVATE_ALL == 64 &&
                   FB_ACTIVATE_FORCE == 128 && FB_ACTIVATE_INV_MODE == 256 &&
                   FB_ACTIVATE_KD_TEXT == 512,
               "fbdev activation flags must match Linux v6.12");

static struct framebuffer device;
static unsigned mode_set_calls;
static unsigned hardware_write_count;
static unsigned trace_change_count;
static bool drm_lease_active;
static unsigned user_read_range_checks;
static unsigned user_write_range_checks;
static bool user_output_writable;

bool user_range_ok(uint64_t address, uint64_t length)
{
    ++user_read_range_checks;
    return address && length == sizeof(struct fb_var_screeninfo);
}

bool user_range_writable(uint64_t address, uint64_t length)
{
    ++user_write_range_checks;
    return user_output_writable && address &&
           length == sizeof(struct fb_var_screeninfo);
}

const struct framebuffer *framebuffer_get(void)
{
    return &device;
}

int framebuffer_set_mode(uint32_t width, uint32_t height)
{
    ++mode_set_calls;
    if (drm_lease_active)
        return device.available && device.width == width && device.height == height
                   ? 0 : -1;
    if (!device.available || !(device.capabilities & FRAMEBUFFER_CAP_MODE_SET) ||
        (device.backend != FRAMEBUFFER_BACKEND_BOCHS_VBE &&
         device.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA) ||
        !width || !height || width > device.max_width || height > device.max_height ||
        (uint64_t)width * height * 4u > device.max_bytes)
        return -1;
    if (device.width == width && device.height == height && device.bpp == 32)
        return 0;

    ++hardware_write_count;
    if (device.backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA)
        ++trace_change_count;
    device.width = width;
    device.height = height;
    device.pitch = width * 4u;
    device.bpp = 32;
    device.bytes_per_pixel = 4;
    device.type = 1;
    device.red_field_position = 16;
    device.red_mask_size = 8;
    device.green_field_position = 8;
    device.green_mask_size = 8;
    device.blue_field_position = 0;
    device.blue_mask_size = 8;
    return 0;
}

static struct fb_var_screeninfo make_var(uint32_t width, uint32_t height,
                                         uint32_t bpp)
{
    struct fb_var_screeninfo var = {0};
    var.xres = width;
    var.yres = height;
    var.xres_virtual = width;
    var.yres_virtual = height;
    var.bits_per_pixel = bpp;
    var.red = (struct fb_bitfield){16, 8, 0};
    var.green = (struct fb_bitfield){8, 8, 0};
    var.blue = (struct fb_bitfield){0, 8, 0};
    return var;
}

static void setup_fb(uint32_t bpp, uint32_t backend)
{
    memset(&device, 0, sizeof(device));
    device.pixels = (uint32_t *)(uintptr_t)0x100000;
    device.width = 800;
    device.height = 600;
    device.pitch = bpp == 24 ? 2400 : 3200;
    device.max_width = 4096;
    device.max_height = 4096;
    device.max_bytes = 64u * 1024u * 1024u;
    device.backend = backend;
    device.capabilities = FRAMEBUFFER_CAP_MODE_SET;
    device.bpp = (uint8_t)bpp;
    device.bytes_per_pixel = bpp == 24 ? 3 : 4;
    device.type = 1;
    device.red_field_position = 16;
    device.red_mask_size = 8;
    device.green_field_position = 8;
    device.green_mask_size = 8;
    device.blue_field_position = 0;
    device.blue_mask_size = 8;
    device.available = true;
    mode_set_calls = 0;
    hardware_write_count = 0;
    trace_change_count = 0;
    drm_lease_active = false;
    user_read_range_checks = 0;
    user_write_range_checks = 0;
    user_output_writable = true;
}

static void assert_xrgb8888_fields(const struct fb_var_screeninfo *var)
{
    assert(var->red.offset == 16 && var->red.length == 8 &&
           var->red.msb_right == 0);
    assert(var->green.offset == 8 && var->green.length == 8 &&
           var->green.msb_right == 0);
    assert(var->blue.offset == 0 && var->blue.length == 8 &&
           var->blue.msb_right == 0);
    assert(var->transp.offset == 0 && var->transp.length == 0 &&
           var->transp.msb_right == 0);
}

static void assert_no_side_effects(const struct framebuffer *before,
                                   unsigned setters, unsigned writes,
                                   unsigned trace_changes)
{
    assert(memcmp(&device, before, sizeof(device)) == 0);
    assert(mode_set_calls == setters);
    assert(hardware_write_count == writes);
    assert(trace_change_count == trace_changes);
}

static void assert_rejected_without_side_effects(struct fb_var_screeninfo *var)
{
    struct framebuffer before = device;
    struct fb_var_screeninfo requested = *var;
    unsigned setters = mode_set_calls;
    unsigned writes = hardware_write_count;
    unsigned traces = trace_change_count;
    assert(framebuffer_fbdev_put_var(var) == -LINUX_EINVAL);
    assert(memcmp(var, &requested, sizeof(requested)) == 0);
    assert_no_side_effects(&before, setters, writes, traces);
}

static void test_test_activation_validates_without_applying(void)
{
    struct framebuffer before;
    struct fb_var_screeninfo var;

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    before = device;
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST | FB_ACTIVATE_KD_TEXT;
    var.pixclock = 39721;
    var.right_margin = 40;
    var.hsync_len = 128;
    var.left_margin = 88;
    var.lower_margin = 1;
    var.vsync_len = 4;
    var.upper_margin = 23;
    var.sync = 3;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == FB_ACTIVATE_TEST);
    assert(var.bits_per_pixel == 24 && var.xres == 800 && var.yres == 600);
    assert(var.xres_virtual == 800 && var.yres_virtual == 600);
    assert(var.red.offset == 16 && var.red.length == 8 && var.red.msb_right == 0);
    assert(var.green.offset == 8 && var.green.length == 8 && var.green.msb_right == 0);
    assert(var.blue.offset == 0 && var.blue.length == 8 && var.blue.msb_right == 0);
    assert(var.transp.offset == 0 && var.transp.length == 0 && var.transp.msb_right == 0);
    assert(var.pixclock == 39721 && var.right_margin == 40 &&
           var.hsync_len == 128 && var.left_margin == 88 &&
           var.lower_margin == 1 && var.vsync_len == 4 &&
           var.upper_margin == 23 && var.sync == 3);
    assert_no_side_effects(&before, 0, 0, 0);

    var = make_var(1024, 768, 32);
    var.activate = FB_ACTIVATE_TEST;
    before = device;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == FB_ACTIVATE_TEST && var.xres == 1024 && var.yres == 768);
    assert(var.bits_per_pixel == 32);
    assert_xrgb8888_fields(&var);
    assert_no_side_effects(&before, 0, 0, 0);

    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_NXTOPEN;
    before = device;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == FB_ACTIVATE_NXTOPEN);
    assert_no_side_effects(&before, 0, 0, 0);
}

static void test_force_modifier_follows_activation_mask(void)
{
    struct framebuffer before;
    struct fb_var_screeninfo var;

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    before = device;
    var = make_var(1024, 768, 32);
    var.activate = FB_ACTIVATE_TEST | FB_ACTIVATE_FORCE | FB_ACTIVATE_KD_TEXT;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == (FB_ACTIVATE_TEST | FB_ACTIVATE_FORCE));
    assert(var.xres == 1024 && var.yres == 768 &&
           var.xres_virtual == 1024 && var.yres_virtual == 768 &&
           var.bits_per_pixel == 32);
    assert_xrgb8888_fields(&var);
    assert_no_side_effects(&before, 0, 0, 0);

    var = make_var(1024, 768, 32);
    var.activate = FB_ACTIVATE_NXTOPEN | FB_ACTIVATE_FORCE;
    before = device;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == (FB_ACTIVATE_NXTOPEN | FB_ACTIVATE_FORCE));
    assert(var.xres == 1024 && var.yres == 768 && var.bits_per_pixel == 32);
    assert_xrgb8888_fields(&var);
    assert_no_side_effects(&before, 0, 0, 0);

    /* The only supported 24bpp request remains the actual active layout. */
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    before = device;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == (FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE));
    assert(var.xres == 800 && var.yres == 600 && var.bits_per_pixel == 24);
    assert_no_side_effects(&before, 0, 0, 0);

    /* FORCE bypasses the helper's same-layout shortcut and reaches the
     * existing setter, whose same-mode 32bpp fast path performs no writes. */
    setup_fb(32, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    before = device;
    var = make_var(800, 600, 32);
    var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == (FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE));
    assert(var.xres == 800 && var.yres == 600 && var.bits_per_pixel == 32);
    assert_no_side_effects(&before, 1, 0, 0);
}

static void test_now_current_mode_is_a_noop(void)
{
    struct framebuffer before;
    struct fb_var_screeninfo var;

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    before = device;
    var = make_var(800, 600, 24);
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.activate == FB_ACTIVATE_NOW && var.bits_per_pixel == 24);
    assert_no_side_effects(&before, 0, 0, 0);

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    device.max_bytes = 1500000;
    before = device;
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert_no_side_effects(&before, 0, 0, 0);

    setup_fb(32, FRAMEBUFFER_BACKEND_VMWARE_SVGA);
    before = device;
    var = make_var(800, 600, 32);
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(var.bits_per_pixel == 32);
    assert_no_side_effects(&before, 0, 0, 0);
}

static void test_now_32bpp_mode_change_returns_actual_layout(void)
{
    struct fb_var_screeninfo var;

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    var = make_var(1024, 768, 32);
    var.pixclock = 39721;
    var.right_margin = 40;
    var.hsync_len = 128;
    var.left_margin = 88;
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(device.width == 1024 && device.height == 768);
    assert(device.bpp == 32 && device.bytes_per_pixel == 4 && device.pitch == 4096);
    assert(mode_set_calls == 1 && hardware_write_count == 1);
    assert(var.xres == device.width && var.yres == device.height);
    assert(var.xres_virtual == device.width && var.yres_virtual == device.height);
    assert(var.bits_per_pixel == device.bpp && var.activate == FB_ACTIVATE_NOW);
    assert_xrgb8888_fields(&var);
    assert(var.pixclock == 39721 && var.right_margin == 40 &&
           var.hsync_len == 128 && var.left_margin == 88);

    var = make_var(1280, 720, 32);
    assert(framebuffer_fbdev_put_var(&var) == 0);
    assert(device.width == 1280 && device.height == 720 && device.pitch == 5120);
    assert(mode_set_calls == 2 && hardware_write_count == 2);
}

static void test_lease_noop_does_not_claim_a_format_change(void)
{
    struct framebuffer before;
    struct fb_var_screeninfo var = make_var(800, 600, 32);
    struct fb_var_screeninfo requested = var;

    setup_fb(24, FRAMEBUFFER_BACKEND_VMWARE_SVGA);
    before = device;
    drm_lease_active = true;
    assert(framebuffer_fbdev_put_var(&var) == -LINUX_EINVAL);
    assert(memcmp(&var, &requested, sizeof(var)) == 0);
    assert_no_side_effects(&before, 1, 0, 0);
}

static void test_lease_rejects_different_size_without_scanout_side_effects(void)
{
    struct framebuffer before;
    struct fb_var_screeninfo var = make_var(1024, 768, 32);
    struct fb_var_screeninfo requested = var;

    setup_fb(24, FRAMEBUFFER_BACKEND_VMWARE_SVGA);
    before = device;
    drm_lease_active = true;
    assert(framebuffer_fbdev_put_var(&var) == -LINUX_EINVAL);
    assert(memcmp(&var, &requested, sizeof(var)) == 0);
    /* One mode-set attempt reaches the lease guard; it performs no write or
     * SVGA trace transition and leaves the complete device state untouched. */
    assert_no_side_effects(&before, 1, 0, 0);
}

static void test_ioctl_validates_before_requesting_copyout(void)
{
    struct framebuffer before;
    struct fb_var_screeninfo var = make_var(800, 600, 16);
    struct fb_var_screeninfo requested = var;

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    before = device;
    user_output_writable = false;
    assert(syscall_fbdev_put_var((uintptr_t)&var) == -LINUX_EINVAL);
    assert(user_read_range_checks == 1 && user_write_range_checks == 0);
    assert(memcmp(&var, &requested, sizeof(var)) == 0);
    assert_no_side_effects(&before, 0, 0, 0);

    user_output_writable = true;
    user_read_range_checks = 0;
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST;
    assert(syscall_fbdev_put_var((uintptr_t)&var) == 0);
    assert(user_read_range_checks == 1 && user_write_range_checks == 1);
    assert(var.activate == FB_ACTIVATE_TEST && var.bits_per_pixel == 24);
    assert_no_side_effects(&before, 0, 0, 0);

    user_output_writable = false;
    user_read_range_checks = 0;
    user_write_range_checks = 0;
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST | FB_ACTIVATE_KD_TEXT;
    requested = var;
    assert(syscall_fbdev_put_var((uintptr_t)&var) == -LINUX_EFAULT);
    assert(user_read_range_checks == 1 && user_write_range_checks == 1);
    assert(memcmp(&var, &requested, sizeof(var)) == 0);
    assert_no_side_effects(&before, 0, 0, 0);
}

static void test_invalid_formats_ranges_and_activation_are_rejected(void)
{
    struct fb_var_screeninfo var;

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    var = make_var(1024, 768, 24);
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 16);
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.red.offset = 15;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.red.msb_right = 1;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.transp.length = 8;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.nonstd = 1;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.grayscale = 1;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.xres_virtual = 799;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.xoffset = 1;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.reserved[2] = 1;
    assert_rejected_without_side_effects(&var);

    var = make_var(800, 600, 24);
    var.activate = 3;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_INV_MODE;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST | FB_ACTIVATE_VBL;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST | FB_ACTIVATE_FORCE | FB_ACTIVATE_VBL;
    assert_rejected_without_side_effects(&var);
    var = make_var(800, 600, 24);
    var.activate = FB_ACTIVATE_TEST | FB_ACTIVATE_ALL;
    assert_rejected_without_side_effects(&var);

    var = make_var(UINT32_MAX, 600, 32);
    var.activate = FB_ACTIVATE_TEST;
    assert_rejected_without_side_effects(&var);

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    device.capabilities = 0;
    var = make_var(1024, 768, 32);
    var.activate = FB_ACTIVATE_TEST;
    assert_rejected_without_side_effects(&var);

    setup_fb(24, FRAMEBUFFER_BACKEND_BOCHS_VBE);
    device.max_width = UINT32_MAX;
    device.max_height = UINT32_MAX;
    device.max_bytes = UINT32_MAX;
    var = make_var(0x80000000u, 0x80000000u, 32);
    var.activate = FB_ACTIVATE_TEST;
    assert_rejected_without_side_effects(&var);
}

int main(void)
{
    test_test_activation_validates_without_applying();
    test_force_modifier_follows_activation_mask();
    test_now_current_mode_is_a_noop();
    test_now_32bpp_mode_change_returns_actual_layout();
    test_lease_noop_does_not_claim_a_format_change();
    test_lease_rejects_different_size_without_scanout_side_effects();
    test_ioctl_validates_before_requesting_copyout();
    test_invalid_formats_ranges_and_activation_are_rejected();
    puts("fbdev-abi: PASS");
    return 0;
}
