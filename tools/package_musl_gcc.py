#!/usr/bin/env python3
"""Package the pinned, unchanged Dyne static Linux musl toolchain."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE_NAME = "dyne-gcc-musl-x86_64.tar.xz"
ARCHIVE_SHA256 = "31420e4f978e7ccbcc597ca5e18c2dcbe640ea6985ed6325112d8a171a72ece3"
ARCHIVE_URL = "https://github.com/dyne/musl/releases/download/2.2.0/" + ARCHIVE_NAME
TOOLS = ("addr2line", "ar", "as", "c++", "c++filt", "cc", "cpp", "elfedit", "g++",
         "gcc", "gcc-15.1.0", "gcc-ar", "gcc-nm", "gcc-ranlib", "gcov", "gcov-dump",
         "gcov-tool", "gprof", "ld", "ld.bfd", "nm", "objcopy", "objdump", "ranlib",
         "readelf", "size", "strings", "strip")
COMMANDS = (*TOOLS, *("x86_64-linux-musl-" + name for name in TOOLS), "musl-gcc", "musl-g++")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def obtain_archive(archive: Path, source: Path | None) -> None:
    archive.parent.mkdir(parents=True, exist_ok=True)
    if source is not None:
        if digest(source) != ARCHIVE_SHA256:
            raise ValueError("LEONOS_GCC_ARCHIVE does not match the pinned Dyne 2.2.0 SHA256")
        if source.resolve() != archive.resolve():
            shutil.copy2(source, archive)
    elif not archive.exists():
        with tempfile.TemporaryDirectory(prefix=".gcc-download-", dir=archive.parent) as directory:
            candidate = Path(directory) / ARCHIVE_NAME
            subprocess.run(["curl", "--fail", "--location",
                            "--retry", "3", "--connect-timeout", "30", "--max-time", "600",
                            "--output", str(candidate), ARCHIVE_URL], check=True)
            if digest(candidate) != ARCHIVE_SHA256:
                raise ValueError("downloaded musl GCC archive failed SHA256 verification")
            candidate.replace(archive)
    if digest(archive) != ARCHIVE_SHA256:
        raise ValueError(f"musl GCC cache SHA256 mismatch: {archive}")


def package(archive: Path, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".musl-gcc-", dir=out.parent) as directory:
        work = Path(directory)
        extracted = work / "extract"
        with tarfile.open(archive) as stream:
            stream.extractall(extracted, filter="data")
        tree = work / "root"
        # Materialize aliases for the installer's file copier while preserving
        # every pathname (including case-distinct headers) and original bytes.
        shutil.copytree(extracted / "dyne", tree / "opt/dyne")
        suite = tree / "opt/dyne/gcc-musl"
        binaries = suite / "bin"
        if {p.name for p in binaries.iterdir()} != {"x86_64-linux-musl-" + name for name in TOOLS}:
            raise ValueError("unexpected compiler command inventory")
        hashes = {str(p.relative_to(tree)): digest(p) for p in sorted(tree.rglob("*")) if p.is_file()}
        for name in TOOLS:
            headers = subprocess.check_output(["readelf", "-l", "-d", binaries / ("x86_64-linux-musl-" + name)], text=True)
            if "INTERP" in headers or "(NEEDED)" in headers:
                raise ValueError(f"upstream command is not static: {name}")
        # One relocatable launcher derives the real x86_64-linux-musl-* name
        # from argv[0].  /usr/bin command entries are relative symlinks to it,
        # created by the staging layout step.
        launcher_dir = tree / "opt/dyne/bin"
        launcher_dir.mkdir(parents=True)
        launcher = launcher_dir / "reliefos-musl-cc"
        subprocess.run([str(binaries / "x86_64-linux-musl-gcc"),
                        "--sysroot=" + str(suite / "x86_64-linux-musl"), "-static", "-Os", "-s",
                        "-Wall", "-Wextra", "-Werror", str(ROOT / "userland/musl-gcc/launcher.c"),
                        "-o", str(launcher)], check=True)
        notices = tree / "usr/share/licenses/musl-gcc"
        notices.mkdir(parents=True)
        for name in ("README.rst", "COPYING3", "COPYING.RUNTIME"):
            shutil.copy2(ROOT / "userland/musl-gcc" / name, notices / name)
        shutil.copy2(ROOT / "third_party/musl/COPYRIGHT", notices / "musl-COPYRIGHT")
        examples = tree / "usr/share/examples/musl-gcc"
        examples.mkdir(parents=True)
        shutil.copy2(ROOT / "userland/musl-gcc/hello.c", examples / "hello.c")
        (tree / ".leonos-package.json").write_text(json.dumps({
            "package": "musl-gcc", "release": "dyne/musl 2.2.0", "gcc": "15.1.0",
            "url": ARCHIVE_URL, "archive_sha256": ARCHIVE_SHA256,
            "target": "x86_64-linux-musl", "host_isa": "x86-64-v2", "linkage": "static",
            "commands": COMMANDS, "upstream_files": hashes,
        }, indent=2) + "\n")
        shutil.copy2(tree / ".leonos-package.json", notices / "package.json")
        if out.exists(): shutil.rmtree(out)
        tree.replace(out)
    print(f"musl-gcc: packaged {len(hashes)} upstream files and {len(COMMANDS)} /usr/bin aliases")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    obtain_archive(args.archive.resolve(), args.source)
    package(args.archive.resolve(), args.out.resolve())


if __name__ == "__main__":
    main()
