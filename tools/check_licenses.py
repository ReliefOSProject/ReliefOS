#!/usr/bin/env python3
"""Check ReliefOS third-party license and attribution packaging policy.

The checker deliberately uses an explicit policy instead of guessing from
arbitrary filenames.  It can inspect source trees, staging directories and
ZIP archives; raw FAT/VMDK images are reported as skipped because silently
claiming that an opaque image is complete would hide packaging mistakes.
"""

from __future__ import annotations

import argparse
import configparser
import json
import re
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


DEFAULT_EXCLUDED_CREDITS = ("llama2.c", "TinyLlama", "karpathy")

# The first existing candidate is accepted.
SUBMODULE_LICENSES: dict[str, tuple[str, ...]] = {
    "litehtml": ("LICENSE",),
    "mbedtls": ("LICENSE",),
    "musl": ("COPYRIGHT",),
    "mimalloc": ("LICENSE",),
    "busybox": ("LICENSE",),
    "ncurses": ("COPYING",),
    "pl_editor": ("LICENSE", "COPYING"),
    "zlib": ("LICENSE",),
    "libpng": ("LICENSE",),
    "stardustui": ("LICENSE",),
    "sl": ("LICENSE",),
    "sqlite": ("LICENSE.md", "LICENSE", "COPYING"),
    "portablegl": ("LICENSE",),
}

# Program directories are conditional: a disabled component is not a missing
# license, while an executable that made it into an image must have its notice.
# file, less and vim arrive as signed upstream Alpine packages now, so their
# notices travel with the package metadata in /lib/apk/db/installed instead.
IMAGE_LICENSES: dict[str, tuple[str, ...]] = {
    "busybox": ("LICENSE",),
    "fastfetch": ("LICENSE",),
    "sl": ("LICENSE",),
    "pleditor": ("LICENSE",),
}

SDK_LICENSES: dict[str, tuple[str, ...]] = {
    "libncursesw.a": ("THIRD_PARTY/NCURSES-COPYING",),
    "libtinfow.a": ("THIRD_PARTY/NCURSES-COPYING",),
    "libc.a": ("THIRD_PARTY/MUSL-COPYING",),
    "libz.a": ("THIRD_PARTY/ZLIB-LICENSE",),
    "libpng.a": ("THIRD_PARTY/LIBPNG-LICENSE",),
    "sqlite.a": ("THIRD_PARTY/SQLITE-LICENSE",),
    "sqlite.so.3": ("THIRD_PARTY/SQLITE-LICENSE",),
    "libstardustui.a": ("THIRD_PARTY/STARDUSTUI-LICENSE",),
    "libportablegl.a": ("THIRD_PARTY/PORTABLEGL-LICENSE",),
    "libportablegl.so.1": ("THIRD_PARTY/PORTABLEGL-LICENSE",),
}


@dataclass(frozen=True)
class Finding:
    id: str
    status: str  # pass, warn, fail, skip
    severity: str
    component: str
    expected: str
    found: str
    path: str
    message: str


class Artifact:
    """Read-only view over a directory or ZIP archive."""

    def __init__(self, source: Path):
        self.source = source
        self.archive: zipfile.ZipFile | None = None
        self.root = source if source.is_dir() else None
        if source.is_file() and zipfile.is_zipfile(source):
            self.archive = zipfile.ZipFile(source)
        self.names = set()
        if self.archive:
            self.names = {n.replace("\\", "/").lstrip("./") for n in self.archive.namelist()}

    @property
    def readable(self) -> bool:
        return self.root is not None or self.archive is not None

    def close(self) -> None:
        if self.archive:
            self.archive.close()

    def _candidates(self, relative: str) -> Iterable[str]:
        relative = relative.replace("\\", "/").lstrip("/")
        if self.root:
            yield str(self.root / Path(*relative.split("/")))
            return
        # SDK/installer ZIPs normally have a top-level devtools/ or install/
        # directory.  Accept a unique suffix so callers need not know it.
        if relative in self.names:
            yield relative
        suffix = "/" + relative
        matches = sorted(n for n in self.names if n.endswith(suffix))
        yield from matches

    def find(self, candidates: Sequence[str]) -> str | None:
        for candidate in candidates:
            if self.root:
                path = self.root / Path(*candidate.replace("\\", "/").split("/"))
                if path.is_file() and path.stat().st_size > 0:
                    return str(path)
            elif self.archive:
                names = [candidate] + sorted(n for n in self.names if n.endswith("/" + candidate))
                for name in names:
                    try:
                        info = self.archive.getinfo(name)
                    except KeyError:
                        continue
                    if info.file_size > 0:
                        return name
        return None

    def exists(self, relative: str) -> bool:
        if self.root:
            return (self.root / Path(*relative.replace("\\", "/").split("/"))).is_file()
        if self.archive:
            return relative in self.names or any(n.endswith("/" + relative) for n in self.names)
        return False

    def has_any(self, relatives: Sequence[str]) -> bool:
        return any(self.exists(path) for path in relatives)

    def has_prefix(self, relative: str) -> bool:
        """Return whether a directory-like prefix has at least one entry."""
        relative = relative.replace("\\", "/").strip("/")
        if self.root:
            return (self.root / Path(*relative.split("/"))).is_dir()
        prefix = relative + "/"
        return bool(self.archive and any(name.startswith(prefix) for name in self.names))

    def api_member(self, api_relative: str, member: str) -> bool:
        """Check a member inside ReliefOS's tar-based ``.api`` package."""
        if not self.root:
            return False
        package = self.root / Path(*api_relative.replace("\\", "/").split("/"))
        if not package.is_file():
            return False
        try:
            with tarfile.open(package, "r:") as archive:
                info = archive.getmember(member)
                return info.isfile() and info.size > 0
        except (OSError, tarfile.TarError, KeyError):
            return False


def result(identifier: str, status: str, severity: str, component: str,
           expected: str, found: str, path: str, message: str) -> Finding:
    return Finding(identifier, status, severity, component, expected, found, path, message)


def parse_submodules(root: Path) -> list[tuple[str, Path]]:
    gitmodules = root / ".gitmodules"
    if not gitmodules.is_file():
        return []
    parser = configparser.ConfigParser()
    parser.read(gitmodules, encoding="utf-8")
    entries = []
    for section in parser.sections():
        if not section.startswith("submodule "):
            continue
        path = parser.get(section, "path", fallback="").strip()
        if path:
            entries.append((Path(path), root / path))
    return [(name.name, path) for name, path in entries]


def check_submodules(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for name, path in parse_submodules(root):
        candidates = SUBMODULE_LICENSES.get(name, ("LICENSE", "COPYING", "COPYING.md", "LICENSE.md"))
        if not path.is_dir():
            findings.append(result("submodule-license", "fail", "error", name,
                                   " or ".join(candidates), "missing submodule", str(path),
                                   "submodule directory is not initialized"))
            continue
        found = next((path / candidate for candidate in candidates
                      if (path / candidate).is_file() and (path / candidate).stat().st_size), None)
        if found:
            findings.append(result("submodule-license", "pass", "info", name,
                                   " or ".join(candidates), str(found.relative_to(root)), str(found),
                                   "license file is present and non-empty"))
        else:
            findings.append(result("submodule-license", "fail", "error", name,
                                   " or ".join(candidates), "missing", str(path),
                                   "no accepted license file was found"))
    return findings


def image_base(source: Path) -> Path:
    if source.is_dir():
        for relative in ("install/root", "root/install/root", "install/esp", "root/install/esp"):
            if (source / relative).is_dir():
                return source / relative
    return source


def check_image(source: Path, category: str = "image-license") -> list[Finding]:
    if not source.exists():
        return [result(category, "skip", "info", "image", "staging directory or ZIP", "not found",
                        str(source), "image/installer staging path is unavailable")]
    artifact = Artifact(image_base(source) if source.is_dir() else source)
    try:
        if not artifact.readable:
            return [result(category, "skip", "warning", "image", "readable directory or ZIP", source.suffix,
                            str(source), "raw image format is opaque; mount or export it before checking")]
        findings: list[Finding] = []
        for program, licenses in IMAGE_LICENSES.items():
            # Accept the pre-FHS and Alpine-shaped locations for compatibility
            # review of older media, but require the license in the current
            # named license directory.
            exec_candidates = (
                f"programs/{program}/{program}.elf",
                f"usr/lib/reliefos/apps/{program}/{program}.elf",
                f"usr/lib/leonos/apps/{program}/{program}.elf",
                f"usr/bin/{program}",
                f"bin/{program}",
                f"opt/{program}/{program}.elf",
                f"opt/{program}/{program}",
            )
            present = any(artifact.exists(candidate) for candidate in exec_candidates) or \
                artifact.has_prefix(f"programs/{program}") or \
                artifact.has_prefix(f"usr/lib/reliefos/apps/{program}") or \
                artifact.has_prefix(f"usr/lib/leonos/apps/{program}") or \
                artifact.has_prefix(f"opt/{program}")
            if not present:
                # Do not flag disabled components.
                continue
            expected = " or ".join(f"usr/share/licenses/{program}/{x}" for x in licenses)
            candidates = tuple(f"usr/share/licenses/{program}/{x}" for x in licenses)
            found = artifact.find(candidates)
            findings.append(result(category, "pass" if found else "fail", "info" if found else "error",
                                   program, expected, found or "missing", str(source),
                                   "packaged program license is present" if found else
                                   "program is present but its license file is missing"))
        if artifact.has_prefix("usr/share/terminfo"):
            notice = "usr/share/licenses/ncurses/COPYING"
            found = artifact.find((notice,))
            findings.append(result(category, "pass" if found else "fail",
                                   "info" if found else "error", "ncurses", notice,
                                   found or "missing", str(source), "ncurses runtime license"))
        api_notices = {"oschinpt": ("LICENSE", "ATTRIBUTION.txt")}
        for api, notices in api_notices.items():
            api_file = f"api/{api}.api"
            if not artifact.exists(api_file):
                continue
            for notice in notices:
                relative = f"{api_file}:{notice}"
                found = relative if artifact.api_member(api_file, notice) else None
                findings.append(result(category, "pass" if found else "fail",
                                       "info" if found else "error", f"{api}/{notice}", relative,
                                       found or "missing", str(source),
                                       "API package attribution file is present" if found else
                                       "API package is present but its attribution file is missing"))
        # Notices for content which is not an application directory.
        if artifact.has_any(("usr/lib/reliefos/apps/doom/doom.elf", "usr/lib/leonos/apps/doom/doom.elf", "programs/doom/doom.elf", "programs/doom/doomgeneric.elf", "api/doom.api")):
            found = artifact.find(("usr/share/doc/reliefos/FREEDOOM-COPYING.txt", "usr/lib/reliefos/apps/doom/FREEDOOM-COPYING.txt", "usr/share/doc/leonos/FREEDOOM-COPYING.txt"))
            findings.append(result(category, "pass" if found else "fail", "info" if found else "error",
                                   "freedoom", "usr/share/doc/reliefos/FREEDOOM-COPYING.txt or packaged Doom notice", found or "missing",
                                   str(source), "Freedoom notice is present" if found else "Freedoom content lacks its notice"))
        if not findings:
            findings.append(result(category, "warn", "warning", "image", "packaged third-party programs",
                                   "none detected", str(source), "no known third-party program was detected in this staging tree"))
        return findings
    finally:
        artifact.close()


def check_sdk(source: Path) -> list[Finding]:
    if not source.exists():
        return [result("sdk-license", "skip", "info", "sdk", "directory or ZIP", "not found", str(source),
                        "SDK path is unavailable")]
    artifact = Artifact(source)
    try:
        if not artifact.readable:
            return [result("sdk-license", "skip", "warning", "sdk", "readable directory or ZIP", source.suffix,
                            str(source), "SDK format is not a directory or ZIP archive")]
        findings: list[Finding] = []
        for library, licenses in SDK_LICENSES.items():
            library_path = f"lib/{library}"
            if not artifact.has_any((library_path, f"devtools/{library}")):
                continue
            candidates = tuple(f"{p}" for p in licenses)
            found = artifact.find(candidates)
            findings.append(result("sdk-license", "pass" if found else "fail", "info" if found else "error",
                                   library, " or ".join(candidates), found or "missing", str(source),
                                   "SDK library license is present" if found else
                                   "SDK library is present but its third-party license is missing"))
        if not findings:
            findings.append(result("sdk-license", "warn", "warning", "sdk", "third-party SDK libraries",
                                   "none detected", str(source), "no known third-party SDK library was detected"))
        return findings
    finally:
        artifact.close()


def read_acknowledgement_source(root: Path) -> str:
    source = root / "userland/apps/installer/main.c"
    if not source.is_file():
        return ""
    text = source.read_text(encoding="utf-8", errors="replace")
    start = text.find("static const char acknowledgements_en[]")
    end = text.find("static int text_eq", start if start >= 0 else 0)
    return text[start:end if end >= 0 else None] if start >= 0 else ""


def check_excluded_credits(root: Path, excluded: Sequence[str]) -> list[Finding]:
    text = read_acknowledgement_source(root)
    if not text:
        return [result("excluded-credit", "fail", "error", "installer acknowledgements", "source text",
                        "missing", "userland/apps/installer/main.c", "installer acknowledgement text was not found")]
    findings = []
    for term in excluded:
        present = re.search(re.escape(term), text, re.IGNORECASE) is not None
        findings.append(result("excluded-credit", "fail" if present else "pass",
                               "error" if present else "info", term, "absent from installer acknowledgement page",
                               "present" if present else "absent", "userland/apps/installer/main.c",
                               "excluded project appears in installer acknowledgements" if present else
                               "excluded project is absent from installer acknowledgements"))
    return findings


def locate_default(root: Path, explicit: Path | None, names: Sequence[str]) -> Path:
    if explicit:
        return explicit if explicit.is_absolute() else root / explicit
    for name in names:
        candidate = root / name
        if candidate.exists():
            return candidate
    return root / names[0]


def report(findings: Sequence[Finding], json_path: Path | None, strict: bool) -> int:
    counts = {status: sum(f.status == status for f in findings) for status in ("pass", "warn", "fail", "skip")}
    for f in findings:
        print(f"[{f.status.upper():4}] {f.id:18} {f.component}: {f.message} ({f.found})")
    print(f"Summary: {counts['pass']} passed, {counts['warn']} warnings, {counts['fail']} failures, {counts['skip']} skipped")
    if json_path:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps({"summary": counts, "findings": [asdict(f) for f in findings]},
                                        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 1 if counts["fail"] or (strict and counts["warn"]) else 0


def self_test() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / ".gitmodules").write_text('[submodule "third_party/demo"]\n\tpath = third_party/demo\n\turl = https://example.invalid/demo\n', encoding="utf-8")
        (root / "third_party/demo").mkdir(parents=True)
        (root / "third_party/demo/LICENSE").write_text("MIT\n", encoding="utf-8")
        (root / "userland/apps/installer").mkdir(parents=True)
        (root / "userland/apps/installer/main.c").write_text('static const char acknowledgements_en[] = "ok";\nstatic int text_eq(void);', encoding="utf-8")
        assert check_submodules(root)[0].status == "pass"
        esp_program = root / "build/esp/usr/bin"
        esp_program.mkdir(parents=True)
        (esp_program / "sl").write_bytes(b"elf")
        assert any(f.component == "sl" and f.status == "fail" for f in check_image(root / "build/esp"))
        esp_license = root / "build/esp/usr/share/licenses/sl"
        esp_license.mkdir(parents=True)
        (esp_license / "LICENSE").write_text("MIT\n", encoding="utf-8")
        assert any(f.component == "sl" and f.status == "pass" for f in check_image(root / "build/esp"))
        sdk = root / "sdk"
        (sdk / "lib").mkdir(parents=True)
        (sdk / "lib/libz.a").write_bytes(b"archive")
        assert any(f.component == "libz.a" and f.status == "fail" for f in check_sdk(sdk))
        (sdk / "THIRD_PARTY").mkdir()
        (sdk / "THIRD_PARTY/ZLIB-LICENSE").write_text("zlib\n", encoding="utf-8")
        assert any(f.component == "libz.a" and f.status == "pass" for f in check_sdk(sdk))
        (root / "userland/apps/installer/main.c").write_text(
            'static const char acknowledgements_en[] = "TinyLlama";\nstatic int text_eq(void);',
            encoding="utf-8")
        assert check_excluded_credits(root, ("TinyLlama",))[0].status == "fail"
        (root / "third_party/demo/LICENSE").unlink()
        assert check_submodules(root)[0].status == "fail"
        print("self-test: passed")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help="repository root (default: parent of this script)")
    parser.add_argument("--image", type=Path, help="ESP staging directory or ZIP")
    parser.add_argument("--sdk", type=Path, help="SDK directory or ZIP")
    parser.add_argument("--installer", type=Path, help="installer staging directory or ZIP")
    parser.add_argument("--excluded-credit", action="append", dest="excluded", default=None,
                        help="project name forbidden in installer acknowledgements (repeatable)")
    parser.add_argument("--json", type=Path, dest="json_path", help="write machine-readable report")
    parser.add_argument("--strict", action="store_true", help="also fail when warnings are present")
    parser.add_argument("--self-test", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    root = (args.root or Path(__file__).resolve().parents[1]).resolve()
    image = locate_default(root, args.image, ("build/esp",))
    # The checked-in devtools kit and LeonOS4-Developer-SDK.zip are gone; an SDK
    # is only license-checked when an artifact is given explicitly.
    sdk = args.sdk
    installer = locate_default(root, args.installer, ("build/install", "build/installer-iso"))
    excluded = tuple(args.excluded) if args.excluded else DEFAULT_EXCLUDED_CREDITS
    findings = check_submodules(root)
    findings += check_image(image)
    if sdk:
        findings += check_sdk(sdk if sdk.is_absolute() else root / sdk)
    # Installer ESP is an image staging tree; acknowledgement source is always
    # checked independently so docs containing the same names cannot confuse it.
    findings += check_image(installer, "installer-license")
    findings += check_excluded_credits(root, excluded)
    return report(findings, args.json_path, args.strict)


if __name__ == "__main__":
    raise SystemExit(main())
