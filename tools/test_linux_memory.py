#!/usr/bin/env python3
"""Exercise kernel memory implementation with host physical-page fixtures."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinuxMemoryTests(unittest.TestCase):
    def test_mprotect_linux_reference(self):
        with tempfile.TemporaryDirectory(prefix="reliefos-mprotect-reference-") as tmp:
            executable = str(Path(tmp) / "mprotect-runtime")
            subprocess.run(["cc", "-std=c11", "-O1", "-pthread",
                            "tools/tests/mprotect_runtime_probe.c", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True, timeout=30)

    def test_memory_ownership_and_elf_page_boundaries(self):
        cases = (("physical_pages", []), ("paging_protection", []), ("private_anon_fault", []), ("elf_interpreter", []),
                 ("elf_file_page", ["kernel/reliefnt/mm/page_cache.c"]), ("user_mmap_arena", []),
                 ("mprotect_vma_growth", ["kernel/reliefnt/kernel/reliefnt/sched/sched.c"]),
                 ("brk_collision", ["kernel/reliefnt/kernel/reliefnt/syscall_mm.c"]))
        for name, sources in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="reliefos-mm-") as tmp:
                executable = str(Path(tmp) / name)
                subprocess.run([
                    "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
                    f"tools/tests/{name}_test.c", *sources, "-o", executable,
                ], cwd=ROOT, check=True)
                subprocess.run([executable], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
