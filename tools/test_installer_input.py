"""Regressions for installer storage operations and independent Unix IPC."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from runtime_test_dependencies import INSTALLER_HOST_INCLUDES

ROOT = Path(__file__).resolve().parents[1]


class InstallerInputTests(unittest.TestCase):
    def test_wrapped_labels_preserve_words_utf8_and_reflow(self):
        """Keep all label text visible as width, language and font metrics change."""
        import os
        with tempfile.TemporaryDirectory(prefix="installer-label-wrap-") as tmp:
            executable = Path(tmp) / "label-wrap"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                "tools/tests/installer_label_wrap_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                                "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"})

    def test_boot_preflight_allows_only_explicit_structural_gpt_replacement(self):
        """Exercise real GPT/boot checks without writing during fresh preflight."""
        with tempfile.TemporaryDirectory(prefix="installer-boot-preflight-") as tmp:
            executable = Path(tmp) / "preflight"
            image = Path(tmp) / "disk.raw"
            with image.open("xb") as stream:
                stream.truncate(768 * 1024**2)
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-Wl,--wrap=open,--wrap=ioctl,--wrap=stat,--wrap=lstat,--wrap=setmntent",
                *INSTALLER_HOST_INCLUDES,
                "tools/tests/installer_boot_preflight_test.c",
                "userland/apps/installer/installer_plan.c",
                "userland/runtime/src/bootloader.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable, image], cwd=ROOT, check=True, timeout=15)

    def test_worker_progress_does_not_block_copying(self):
        with tempfile.TemporaryDirectory(prefix="installer-worker-") as tmp:
            executable = str(Path(tmp) / "worker")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-DINSTALLER_JOB_FINISH_TIMEOUT_MS=100",
                "-fsanitize=address,undefined", "tools/tests/installer_job_test.c",
                "userland/apps/installer/installer_job.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_update_sessions_require_complete_metadata_and_terminal_result(self):
        with tempfile.TemporaryDirectory(prefix="installer-update-session-") as tmp:
            executable = str(Path(tmp) / "protocol")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                "-Wl,--gc-sections", "tools/tests/installer_update_protocol_test.c",
                "userland/apps/installer/installer_update_protocol.c",
                "userland/apps/installer/installer_update_model.c",
                "userland/apps/installer/installer_update_progress.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_fresh_copy_metadata_and_failure_safety(self):
        """Exercise real copy metadata and publication on an owned host tree."""
        with tempfile.TemporaryDirectory(prefix="installer-fresh-copy-") as tmp:
            executable = str(Path(tmp) / "fresh-copy")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                *INSTALLER_HOST_INCLUDES,
                "tools/tests/installer_fresh_copy_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable, tmp], cwd=ROOT, check=True, timeout=20)

    def test_capacity_measurement_and_checked_limits(self):
        """Measure actual trees without following links; reject bytes and inode overflow."""
        with tempfile.TemporaryDirectory(prefix="installer-capacity-") as tmp:
            executable = str(Path(tmp) / "capacity")
            subprocess.run([
                "cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
                "tools/tests/installer_capacity_test.c",
                "userland/apps/installer/installer_capacity.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable, tmp], cwd=ROOT, check=True, timeout=20)

    def test_fresh_copy_sync_error_blocks_boot_publication(self):
        """Batch only a fresh root; require its Linux filesystem barrier to pass."""
        with tempfile.TemporaryDirectory(prefix="installer-sync-") as tmp:
            executable = str(Path(tmp) / "copy-sync")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                *INSTALLER_HOST_INCLUDES,
                "tools/tests/installer_copy_sync_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable, tmp], cwd=ROOT, check=True, timeout=20)


    def test_copy_progress_does_not_log_every_short_io_slice(self):
        """Bound presentation overhead while preserving the next timed update."""
        with tempfile.TemporaryDirectory(prefix="installer-progress-") as tmp:
            executable = str(Path(tmp) / "progress")
            subprocess.run([
                "cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                *INSTALLER_HOST_INCLUDES,
                "tools/tests/installer_progress_test.c", "-o", executable,
            ], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=10)

    def test_update_to_fresh_mount_transition(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-installer-mount-") as tmp:
            executable = str(Path(tmp) / "mount-transition")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-Wl,--wrap=open",
                *INSTALLER_HOST_INCLUDES,
                "tools/tests/installer_mount_transition_test.c", "userland/apps/installer/installer_partitions.c", "userland/apps/installer/installer_capacity.c", "userland/apps/installer/installer_plan.c", "-o", executable,
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


if __name__ == "__main__":
    unittest.main()
