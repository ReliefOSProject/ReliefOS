"""Build and run the real SVGA driver against the host device model."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_gpu import GpuAbiTests

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "kernel/reliefnt/drivers/video/svga" / name for name in
           ("fifo.c", "batch_state.c", "device.c", "gmr.c", "gb.c", "present.c", "svga3d.c", "shader.c",
            "borrowed.c", "query.c", "triangle.c", "render.c")]


class SvgaTests(unittest.TestCase):
    def test_driver(self):
        self.assertTrue(all(path.exists() for path in SOURCES),
                        "SVGA3D implementation is missing")
        with tempfile.TemporaryDirectory(prefix="reliefos-svga-") as directory:
            executable = Path(directory) / "svga-test"
            # Scanout/FIFO tests include framebuffer.c but do not render text.
            # Supply a minimal font fixture without requiring a kernel build.
            generated = Path(directory) / "generated"
            generated.mkdir()
            (generated / "cjk_font.h").write_text(
                "static const struct { uint32_t cp; uint8_t width; uint8_t bits[32]; } "
                "cjk_glyphs[] = {{0xfffdU, 1, {0}}};\n")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-DSVGA_HOST_TEST", f"-I{directory}", "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
                "-Ikernel/reliefnt/drivers/video/svga", "tools/tests/svga_test.c",
                *(str(path) for path in SOURCES), "-o", str(executable),
            ], cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True, timeout=30)

    def test_kernel_runtime_dependencies(self):
        # The host ABI hides compiler float helpers that -mgeneral-regs-only
        # emits for the kernel. Check the real target before full OS linking.
        kernel_imports = {
            "console_printf", "console_log", "framebuffer_get", "kernel_spin_lock_irqsave",
            "kernel_spin_unlock_irqrestore", "kernel_irq_save", "kernel_irq_restore",
            "mm_alloc_pages", "mm_free_pages",
            "paging_kernel_direct_map", "paging_kernel_direct_map_range",
            "paging_mmio_uncached",
            "paging_mmio_writeback_nx", "pci_device_bar_contains",
            "drm_device_pci_identity", "x86_64_inl",
            "x86_64_outl", "memcpy", "memset", "memmove", "kernel_malloc",
            "kernel_free",
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
                            "-Ikernel/reliefnt/include", "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-c", str(source),
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
