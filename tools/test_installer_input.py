"""Regression tests for installer mouse initialization and window IPC."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallerInputTests(unittest.TestCase):
    def test_update_to_fresh_mount_transition(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-installer-mount-") as tmp:
            executable = str(Path(tmp) / "mount-transition")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
                "tools/tests/installer_mount_transition_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_keyboard_led_ioctl(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-keyboard-led-") as tmp:
            executable = str(Path(tmp) / "led")
            subprocess.run([
                "clang", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                "tools/tests/evdev_led_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_disk_enumeration_preserves_posix_errors(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-block-errors-") as tmp:
            executable = str(Path(tmp) / "block-errors")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/blockdev_errno_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_window_buffer_follows_presented_size(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-window-resize-") as tmp:
            executable = str(Path(tmp) / "window-resize")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-include", "string.h",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/window_resize_test.c", "userland/runtime/src/unix_ipc.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_window_creation_is_retried_before_present(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-windowd-") as tmp:
            executable = str(Path(tmp) / "windowd-announce")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/windowd_announce_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_disconnected_ipc_clients_are_not_reported_as_would_block(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-ipc-disconnect-") as tmp:
            executable = str(Path(tmp) / "ipc-disconnect")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-include", "sys/un.h",
                "-idirafter", "userland/runtime/include",
                "tools/tests/unix_ipc_disconnect_test.c", "userland/runtime/src/unix_ipc.c",
                "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_mouse_coordinates_survive_evdev_routing(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-pointer-") as tmp:
            executable = str(Path(tmp) / "pointer")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                "-idirafter", "userland/runtime/include",
                "tools/tests/pointer_routing_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_descriptor_receive_preserves_next_frame(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-ipc-") as tmp:
            executable = str(Path(tmp) / "ipc-frames")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-include", "sys/un.h",
                "-idirafter", "userland/runtime/include",
                "tools/tests/unix_ipc_frame_test.c", "userland/runtime/src/unix_ipc.c",
                "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_partial_send_survives_paused_compositor(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-ipc-send-") as tmp:
            executable = str(Path(tmp) / "partial-send")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-fsanitize=address,undefined", "-include", "sys/un.h",
                "-idirafter", "userland/runtime/include",
                "tools/tests/unix_ipc_partial_send_test.c", "userland/runtime/src/unix_ipc.c",
                "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_input_before_fetch_reply_preserves_descriptor(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-wind-") as tmp:
            executable = str(Path(tmp) / "wind-reply")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-idirafter", "userland/runtime/include", "-idirafter", "include", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include/uapi",
                "tools/tests/wind_reply_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_keyboard_irq_rejects_empty_and_auxiliary_output(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-keyboard-irq-") as tmp:
            executable = str(Path(tmp) / "keyboard-irq")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-Ikernel/reliefnt/kernel/reliefnt/include",
                "tools/tests/keyboard_irq_ready_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_mouse_parameters_reach_auxiliary_port(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-mouse-") as tmp:
            executable = str(Path(tmp) / "mouse-init")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                "tools/tests/mouse_init_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
