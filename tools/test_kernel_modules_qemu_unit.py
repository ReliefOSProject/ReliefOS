#!/usr/bin/env python3
"""Do not confuse tagged kernel serial sideband with a normal guest command."""
import unittest
from PIL import Image, ImageDraw, ImageFont
from test_kernel_modules_qemu import guest_output, require_graphical_identity, graphical_marker_visible, session_terminal_focused, focus_session_terminal, wait_graphical_marker


class GuestOutput(unittest.TestCase):
    def test_actual_uname_capture_preserves_the_one_guest_identity(self):
        release = "5.1.2-ko1-95eefb122886"
        trace = ("[ 264.382007] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] exec pid=418 path=/bin/busybox\n"
                 + release + "\n"
                 + "[ 264.517891] \x1b[38;2;128;144;160m[DEBUG] \x1b[0m[reliefnt] scheduler task exited pid=418\n")
        self.assertEqual(guest_output(trace), release + "\n")

    def test_a_trace_containing_the_expected_release_cannot_pass_as_uname(self):
        trace = "[ 1.002] [DEBUG] [reliefnt] loaded kernel=expected-release\nwrong-release\n"
        self.assertEqual(guest_output(trace), "wrong-release\n")

    def test_unexpected_guest_diagnostics_are_not_silently_removed(self):
        self.assertEqual(guest_output("ERROR: wrong package\n[application] warning\n"),
                         "ERROR: wrong package\n[application] warning\n")

    def test_duplicate_identity_lines_and_module_rows_survive(self):
        self.assertEqual(guest_output("release\nrelease\n"), "release\nrelease\n")
        row = "ext4 12345 2 ext_common, Live 0xffffffffa0000000\n"
        self.assertEqual(guest_output(row), row)

    def test_restored_terminal_must_really_have_focus(self):
        frame = Image.new("RGB", (1920, 1080), "black")
        draw = ImageDraw.Draw(frame)
        draw.rectangle((3, 25, 710, 479), fill=(247, 248, 251))
        draw.rectangle((7, 6, 656, 24), fill=(128, 128, 128))
        draw.rectangle((720, 6, 1629, 24), fill=(0, 0, 128))
        # An active Fileman beside a visible st cannot confirm st focus.
        self.assertFalse(session_terminal_focused(frame))
        draw.rectangle((7, 6, 656, 24), fill=(0, 0, 128))
        self.assertTrue(session_terminal_focused(frame))
        self.assertFalse(session_terminal_focused(Image.new("RGB", (640, 480), "black")))

    def test_saved_active_title_cannot_confirm_a_new_focus_request(self):
        class Process:
            def poll(self): return None
        class RecordedDisplay:
            def __init__(self, frames): self.frames = iter(frames); self.last = None
            def key(self, name): pass
            def click(self, x, y): pass
            def frame(self, name):
                self.last = next(self.frames, self.last)
                return self.last
        active = Image.new("RGB", (1920, 1080), "black")
        ImageDraw.Draw(active).rectangle((7, 6, 656, 24), fill=(0, 0, 128))
        blank = Image.new("RGB", (1920, 1080), "black")
        for frames in ([active], [active, blank]):
            with self.subTest(frames=len(frames)), self.assertRaises(AssertionError):
                focus_session_terminal(RecordedDisplay(frames), Process(), timeout=.05)
        inactive = active.copy()
        ImageDraw.Draw(inactive).rectangle((7, 6, 656, 24), fill=(128, 128, 128))
        focus_session_terminal(RecordedDisplay([active, inactive, active]), Process(), timeout=2)

    def test_graphical_marker_requires_output_not_the_typed_command(self):
        marker = "KOREADY87ab3091"
        command = "test@reliefos:~$ (id -u; tty) >/home/test/kr87ab3091; echo " + marker
        self.assertFalse(graphical_marker_visible(command + "\n", marker))
        self.assertTrue(graphical_marker_visible(command + "\n" + marker + "\ntest@reliefos:~$", marker))
        self.assertFalse(graphical_marker_visible("KOREADY87ab3092\n", marker))

    def test_fresh_nonroot_graphical_pty_is_required(self):
        self.assertEqual(require_graphical_identity("1000\n/dev/pts/7\n"),
                         {"uid": 1000, "tty": "/dev/pts/7"})
        for output in ("", "1000", "0\n/dev/tty2\n", "1000\n/dev/tty2\n",
                       "0\n/dev/pts/7\n", "1000\n1000\n/dev/pts/7\n"):
            with self.subTest(output=output), self.assertRaises(AssertionError):
                require_graphical_identity(output)

    def test_other_window_text_cannot_hide_real_terminal_output(self):
        class Process:
            def poll(self): return None
        class Display:
            def __init__(self, frame): self.image = frame
            def frame(self, name): return self.image
        frame = Image.new("RGB", (1920, 1080), "black")
        draw = ImageDraw.Draw(frame)
        draw.rectangle((2, 2, 714, 482), fill=(192, 192, 192))
        draw.rectangle((6, 26, 710, 478), fill="black")
        draw.rectangle((720, 2, 1700, 630), fill="white")
        font = ImageFont.truetype("DejaVuSansMono.ttf", 14)
        draw.text((8, 300), "test@reliefos:~$ echo KOREADY", font=font, fill="white")
        draw.text((8, 318), "KOREADY", font=font, fill="white")
        draw.text((940, 300), "Projects", font=font, fill="black")
        draw.text((940, 318), "modulecheck", font=font, fill="black")
        wait_graphical_marker(Display(frame), Process(), "KOREADY", timeout=.1)
        draw.rectangle((6, 318, 710, 340), fill="black")
        with self.assertRaises(AssertionError):
            wait_graphical_marker(Display(frame), Process(), "KOREADY", timeout=.1)


if __name__ == "__main__":
    unittest.main()
