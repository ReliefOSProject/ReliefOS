"""Verify production FAT directory extension against ASan/UBSan and mtools."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    """Create only a private image, then inspect every generated name externally."""
    with tempfile.TemporaryDirectory(prefix="fat-directory-") as directory:
        work = Path(directory)
        image, executable = work / "fat.img", work / "growth"
        with image.open("wb") as stream:
            stream.truncate(128 * 1024**2)
        subprocess.run(["mkfs.fat", "--invariant", "-F", "32", "-s", "1", image], check=True)
        subprocess.run([
            "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi",
            "tools/tests/fat_directory_growth_test.c", "kernel/reliefnt/fs/tmpfs/core.c",
            "kernel/reliefnt/lib/text_utf16.c", "-o", executable,
        ], cwd=ROOT, check=True)
        subprocess.run([executable, image], check=True, timeout=60)
        subprocess.run(["fsck.fat", "-n", image], check=True)
        names = subprocess.check_output(["mdir", "-b", "-i", image, "::/mods"], text=True).splitlines()
        assert len(names) == 313 and any("gfxterm_menu.mod" in name for name in names)
        print("PASS mtools independently reads all313 short/LFN members; FAT remains clean")


if __name__ == "__main__":
    main()
