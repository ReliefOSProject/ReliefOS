"""Regression checks for the procfs-backed task manager data path."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProcfsTaskmgrTests(unittest.TestCase):
    def test_status_snapshot_consumer(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-procsys-") as directory:
            executable = str(Path(directory) / "procsys")
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
                            "tools/tests/procsys_status_test.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_real_procfs_directory_lookup_and_enumeration(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-procfs-") as directory:
            executable = str(Path(directory) / "procfs")
            subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                            "tools/tests/procfs_directories_test.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_proc_pid_paths_start_after_proc_slash(self):
        source = (ROOT / "kernel/reliefnt/fs/procfs.c").read_text()
        self.assertIn("const char *p = path + 6;", source)

    def test_procfs_exports_cpu_runtime_stats(self):
        source = (ROOT / "kernel/reliefnt/fs/procfs.c").read_text()
        libc = (ROOT / "userland/runtime/src/procsys.c").read_text()
        self.assertIn('proc_text_eq(path, "/proc/stat")', source)
        self.assertIn('ps_read_file("/proc/stat"', libc)

    def test_taskmgr_presents_initial_snapshot(self):
        source = (ROOT / "userland/apps/taskmgr/main.cpp").read_text()
        init = source.index("int main(int argc, char **argv)")
        main = source[init:]
        steps = (
            "reliefos_fltk::init();",
            "taskmgr_perf_history_init(&history);",
            "build_ui();",
            "shell->show(argc, argv);",
            "refresh_performance();",
            "refresh_tasks();",
            "refresh_startup();",
            "update_controls();",
            "Fl::add_timeout(0.5, timer_callback);",
            "return Fl::run();",
        )
        positions = [main.index(step) for step in steps]
        self.assertEqual(positions, sorted(positions))

    def test_taskmgr_service_manager_entry_uses_openrc_manager(self):
        source = (ROOT / "userland/apps/taskmgr/main.cpp").read_text()
        self.assertIn('T("Service Manager")', source)
        self.assertIn('reliefos_launch_builtin_path("servicemgr")', source)
        self.assertIn('reliefos_launch_argv(args)', source)
        self.assertNotIn('T("Startup Apps")', source)

    def test_musl_forkpty_propagates_child_setup_failure(self):
        # forkpty now belongs to the unmodified musl provider; the removed
        # private libc implementation is no longer the runtime under test.
        source = (ROOT / "third_party/musl/src/misc/forkpty.c").read_text()
        self.assertIn("if (login_tty(s))", source)
        self.assertIn("write(p[1], &errno, sizeof errno);", source)
        self.assertIn("waitpid(pid, &status, 0);", source)
        self.assertIn("errno = ec;", source)
        self.assertIn("if (pid > 0) *pm = m;", source)
        self.assertIn("else close(m);", source)

    def test_pty_hangup_preserves_pending_canonical_input(self):
        source = (ROOT / "kernel/reliefnt/kernel/reliefnt/pty.c").read_text()
        start = source.index("int pty_destroy(")
        end = source.index("/**", start)
        destroy = source[start:end]
        self.assertIn("pty_commit_canonical_input(session);", destroy)


if __name__ == "__main__":
    unittest.main()
