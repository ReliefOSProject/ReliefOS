#!/usr/bin/env python3
"""Package the pinned, unmodified static musl CPython installation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "3.14.7"
ARCHIVE_NAME = "cpython-3.14.7+20260901-x86_64-unknown-linux-musl-lto+static-full.tar.zst"
ARCHIVE_SHA256 = "e5a76e5893c39c89ed268ad71c3d6794c3be6b7236ec613449140c57686fc17f"
COMMANDS = ("python", "python3", "python3.14")
PYTHON_LAUNCHER = "usr/bin/python3.14"
# The package tree also carries python/python3 aliases; the image layout stage
# creates those relative symlinks directly so the staging graph owns each
# pathname exactly once.
PAYLOAD_PATHS = ("opt/python", "usr/share/licenses/python",
                 "usr/share/examples/python", PYTHON_LAUNCHER)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def obtain_archive(archive: Path, source: Path | None) -> None:
    archive.parent.mkdir(parents=True, exist_ok=True)
    if source is not None:
        if digest(source) != ARCHIVE_SHA256:
            raise ValueError("LEONOS_PYTHON_ARCHIVE does not match the pinned CPython SHA256")
        if source.resolve() != archive.resolve():
            shutil.copy2(source, archive)
    if not archive.is_file():
        raise FileNotFoundError(f"Provide {ARCHIVE_NAME} with LEONOS_PYTHON_ARCHIVE or cache it at {archive}")
    if digest(archive) != ARCHIVE_SHA256:
        raise ValueError(f"Python archive SHA256 mismatch: {archive}")


def package(archive: Path, out: Path, musl: Path) -> None:
    import musl_link

    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".python-package-", dir=out.parent) as directory:
        work = Path(directory)
        extracted = work / "extract"
        with subprocess.Popen(["zstd", "-dc", str(archive)], stdout=subprocess.PIPE) as unpack:
            with tarfile.open(fileobj=unpack.stdout, mode="r|") as stream:
                members = (member for member in stream if
                           member.name.startswith(("python/install/", "python/licenses/")) or
                           member.name == "python/PYTHON.json")
                stream.extractall(extracted, members=members, filter="data")
            if unpack.wait() != 0:
                raise RuntimeError("Python archive decompression failed")
        metadata = json.loads((extracted / "python/PYTHON.json").read_text())
        if metadata["python_version"] != VERSION or metadata["target_triple"] != "x86_64-unknown-linux-musl":
            raise ValueError("Unexpected Python version or target")
        tree = work / "root"
        # The installer copies regular files, so materialize aliases as GCC does.
        shutil.copytree(extracted / "python/install", tree / "opt/python")
        binary = tree / "opt/python/bin/python3.14"
        headers = subprocess.check_output(["readelf", "-l", "-d", binary], text=True)
        if "INTERP" in headers or "(NEEDED)" in headers:
            raise ValueError("Python executable is not static")
        notices = tree / "usr/share/licenses/python"
        shutil.copytree(extracted / "python/licenses", notices)
        shutil.copy2(extracted / "python/PYTHON.json", notices / "PYTHON.json")
        shutil.copy2(ROOT / "userland/python/README.rst", notices / "README.rst")
        hashes = {p.relative_to(tree).as_posix(): digest(p)
                  for p in sorted((tree / "opt/python").rglob("*")) if p.is_file()}
        launcher = tree / PYTHON_LAUNCHER
        launcher.parent.mkdir(parents=True)
        obj = work / "launcher.o"
        subprocess.run(["clang", "--target=x86_64-linux-musl", "-O2", "-fno-stack-protector",
                        "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
                        "-nostdinc", "-isystem", str(musl / "include"), "-c",
                        str(ROOT / "userland/python/launcher.c"), "-o", str(obj)], check=True)
        subprocess.run(musl_link.executable(musl, launcher, (obj,), static=True, flags=("-s",)), check=True)
        # Aliases are real relative symlinks; the installer and ext2 image
        # builders preserve them instead of materializing duplicate files.
        for name in COMMANDS:
            if name == "python3.14":
                continue
            link = tree / "usr/bin" / name
            link.symlink_to("python3.14")
        examples = tree / "usr/share/examples/python"
        examples.mkdir(parents=True)
        shutil.copy2(ROOT / "userland/python/hello.py", examples / "hello.py")
        manifest = {"package": "python", "version": VERSION, "archive": ARCHIVE_NAME,
                    "archive_sha256": ARCHIVE_SHA256, "target": metadata["target_triple"],
                    "linkage": "static", "commands": COMMANDS, "upstream_files": hashes}
        (tree / ".leonos-package.json").write_text(json.dumps(manifest, indent=2) + "\n")
        shutil.copy2(tree / ".leonos-package.json", notices / "package.json")
        if out.exists():
            shutil.rmtree(out)
        tree.replace(out)
    print(f"Python {VERSION}: packaged complete installation, {len(hashes)} upstream files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--musl", required=True, type=Path)
    args = parser.parse_args()
    obtain_archive(args.archive.resolve(), args.source)
    package(args.archive.resolve(), args.out.resolve(), args.musl.resolve())
