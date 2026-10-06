"""Build and run the real SVGA driver against the host device model."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_gpu import GpuAbiTests

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "kernel/reliefnt/drivers/bootstrap/svga" / name for name in
           ("fifo.c", "device.c", "gmr.c", "gb.c", "svga3d.c", "triangle.c", "render.c")]


class SvgaTests(unittest.TestCase):
    def test_driver(self):
        self.assertTrue(all(path.exists() for path in SOURCES),
                        "SVGA3D implementation is missing")
        with tempfile.TemporaryDirectory(prefix="reliefos-svga-") as directory:
            executable = Path(directory) / "svga-test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-DSVGA_HOST_TEST", "-Ikernel/reliefnt/kernel/reliefnt/include", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-Ikernel/reliefnt/drivers/bootstrap/svga", "tools/tests/svga_test.c",
                *(str(path) for path in SOURCES), "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True, timeout=30)

    def test_kernel_runtime_dependencies(self):
        # The host ABI hides compiler float helpers that -mgeneral-regs-only
        # emits for the kernel. Check the real target before full OS linking.
        kernel_imports = {
            "console_printf", "framebuffer_get", "kernel_spin_lock_irqsave",
            "kernel_spin_unlock_irqrestore", "mm_alloc_pages", "mm_free_pages",
            "paging_kernel_direct_map", "paging_mmio_uncached", "x86_64_inl",
            "x86_64_outl", "memcpy", "memset", "memmove",
        }
        with tempfile.TemporaryDirectory(prefix="reliefos-svga-kernel-") as directory:
            for optimization in ("-O0", "-O2"):
                with self.subTest(optimization=optimization):
                    objects = []
                    for source in SOURCES:
                        obj = Path(directory) / (source.stem + ".o")
                        subprocess.run([
                            "clang", "-target", "x86_64-unknown-none", optimization,
                            "-std=c11", "-ffreestanding", "-fno-stack-protector",
                            "-fno-pic", "-fno-pie", "-mno-red-zone", "-mcmodel=kernel",
                            "-mgeneral-regs-only", "-Wall", "-Wextra", "-Werror",
                            "-Ikernel/reliefnt/kernel/reliefnt/include", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-c", str(source),
                            "-o", str(obj),
                        ], cwd=ROOT, check=True)
                        objects.append(str(obj))
                    combined = Path(directory) / "svga.o"
                    subprocess.run(["ld.lld", "-r", "-o", str(combined), *objects],
                                   cwd=ROOT, check=True)
                    result = subprocess.run(["nm", "-u", str(combined)],
                                            check=True, text=True, capture_output=True)
                    undefined = {line.split()[-1] for line in result.stdout.splitlines()}
                    self.assertFalse(undefined - kernel_imports,
                                     f"Unavailable kernel imports: {undefined - kernel_imports}")


if __name__ == "__main__":
    unittest.main()
