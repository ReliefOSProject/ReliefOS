#!/usr/bin/env python3
"""Regression checks for mmap-backed fbdev refresh without SVGA traces."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FRAMEBUFFER = ROOT / "kernel/reliefnt/drivers/bootstrap/framebuffer.c"
FRAMEBUFFER_H = ROOT / "kernel/reliefnt/kernel/reliefnt/include/reliefnt/framebuffer.h"
CONSOLE = ROOT / "kernel/reliefnt/drivers/bootstrap/console.c"
CONSOLE_H = ROOT / "kernel/reliefnt/kernel/reliefnt/include/reliefnt/console.h"
TIME = ROOT / "kernel/reliefnt/kernel/reliefnt/time.c"


class DisplayRefreshTests(unittest.TestCase):
    def test_present_path_serializes_fifo_and_exposes_tick_refresh(self):
        source = FRAMEBUFFER.read_text()
        header = FRAMEBUFFER_H.read_text()
        self.assertIn("uint64_t flags = svga_lock();", source)
        self.assertIn("svga_unlock(flags);", source)
        self.assertIn("void framebuffer_display_tick(void);", header)

    def test_graphical_console_refresh_is_driven_from_timer(self):
        console = CONSOLE.read_text()
        console_header = CONSOLE_H.read_text()
        timer = TIME.read_text()
        self.assertIn("void console_display_tick(void)", console)
        self.assertIn("void console_display_tick(void);", console_header)
        self.assertIn("console_display_tick();", timer)

    def test_graphical_refresh_is_independent_of_text_console_owner(self):
        console = CONSOLE.read_text()
        tick = console.split("void console_display_tick(void)", 1)[1].split(
            "static void print_unsigned_raw", 1)[0]
        self.assertIn("if (!console_vt_graphical || !framebuffer_get()->available)", tick)
        self.assertNotIn("!fb_console_enabled", tick)

        activate = console.split("void console_vt_activate", 1)[1].split(
            "void console_printf", 1)[0]
        self.assertIn("if (!framebuffer_get()->available) return;", activate)
        self.assertIn("if (graphical)", activate)
        self.assertIn("if (!fb_console_enabled) return;", activate)

    def test_present_clips_regions_before_extended_svga_update(self):
        source = FRAMEBUFFER.read_text()
        present = source.split("void framebuffer_present_region", 1)[1].split(
            "void framebuffer_present(void)", 1)[0]
        self.assertLess(present.index("if (x >= fb.width || y >= fb.height)"),
                        present.index("if (svga.available)"))
        self.assertLess(present.index("if (!width || !height)"),
                        present.index("if (svga.available)"))

    def test_legacy_fifo_update_does_not_reenter_svga_lock(self):
        source = FRAMEBUFFER.read_text()
        fifo_update = source.split(
            "static int framebuffer_vmware_fifo_update", 1
        )[1].split("/* Kick the host", 1)[0]
        self.assertIn("framebuffer_fifo_reserve", fifo_update)
        self.assertNotIn("svga_update(", fifo_update)

    def test_mode_switch_restores_mmap_trace_updates(self):
        source = FRAMEBUFFER.read_text()
        mode = source.split("static int framebuffer_vmware_set_mode", 1)[1].split(
            "static uint8_t *framebuffer_pixel_ptr", 1)[0]
        self.assertIn("framebuffer_vmware_enable_traces();", mode)


if __name__ == "__main__":
    unittest.main()
