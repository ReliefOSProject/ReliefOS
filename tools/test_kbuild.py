#!/usr/bin/env python3
"""Exercise the vendored Kbuild itself with real out-of-tree host/C/ASM builds."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd, *, success=True):
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=60)
    if (result.returncode == 0) != success:
        raise AssertionError(result.stdout)
    return result.stdout


def main():
    manifest = ROOT / "scripts/kbuild-source.json"
    if not manifest.is_file():
        raise AssertionError("the pinned Linux Kbuild source manifest is missing")
    source = json.loads(manifest.read_text())
    for name, digest in source["files"].items():
        path = ROOT / name
        assert path.is_file(), name
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
    temporary = Path(os.environ.get("TMPDIR", str(ROOT / "out/tmp"))).resolve()
    temporary.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("clang") or shutil.which("cc")
    archive = shutil.which("llvm-ar") or shutil.which("ar")
    assert compiler and archive, "host C compiler and ar are required"
    with tempfile.TemporaryDirectory(prefix="kbuild-", dir=temporary) as directory:
        work = Path(directory)
        src, out, other = (work / name for name in ("src", "obj", "other"))
        for path in (src, out, other):
            path.mkdir()
        (src / "scripts").symlink_to(ROOT / "scripts", target_is_directory=True)
        (src / "Kbuild").write_text("obj-y := first.o second.o start.o sub/\nhostprogs-always-y := helper\n")
        (src / "answer.h").write_text("#define ANSWER 41\n")
        (src / "first.c").write_text('#include "answer.h"\n#ifdef FAIL_COMPILE\n#error requested compiler failure\n#endif\nint first(void) { return ANSWER; }\n')
        (src / "second.c").write_text("int second(void) { return 2; }\n")
        (src / "start.S").write_text('.text\n.globl entry\nentry: ret\n.section .note.GNU-stack,"",@progbits\n')
        (src / "helper.c").write_text("int main(void) { return 0; }\n")
        (src / "sub").mkdir()
        (src / "sub/Kbuild").write_text("obj-y := third.o\n")
        (src / "sub/third.c").write_text("int third(void) { return 3; }\n")
        log = work / "compiler.log"
        wrapper = work / "cc"
        wrapper.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "' + str(log) + '"\nexec "' + compiler + '" "$@"\n')
        wrapper.chmod(0o755)

        def make(output, obj=".", flags="-O2"):
            return ["make", "--no-print-directory", "-rR", "-j2", "-f",
                    str(ROOT / "scripts/Makefile.port"), "-f",
                    str(ROOT / "scripts/Makefile.build"), "srctree=" + str(src),
                    "objtree=" + str(output), "VPATH=" + str(src),
                    "obj=" + obj, "KBUILD_BUILTIN=1", "need-builtin=1", "CC=" + str(wrapper),
                    "AR=" + archive, "HOSTCC=" + compiler,
                    "KBUILD_HOSTCFLAGS=-O2 -I" + str(ROOT / "scripts/include"),
                    "KBUILD_CFLAGS=" + flags,
                    "AWK=awk", "NM=nm", "CONFIG_SHELL=/bin/sh", "quiet=quiet_",
                    "RELIEFOS_CC_ID=", "RELIEFOS_CXX_ID=", "RELIEFOS_LD_ID=", "RELIEFOS_AR_ID="]

        def lines():
            return log.read_text().splitlines() if log.exists() else []

        def build(output=out, flags="-O2"):
            run(make(output, "scripts/basic", flags), output)
            return run(make(output, flags=flags), output)

        build()
        run([str(out / "helper")], out)
        assert len(lines()) == 4, lines()
        assert (out / ".first.o.cmd").is_file()
        assert "answer.h" in (out / ".first.o.cmd").read_text()
        assert (out / "scripts/basic/fixdep").is_file()
        assert len(run([archive, "t", "built-in.a"], out).splitlines()) == 4
        before = lines()
        build()
        assert lines() == before, "unchanged build compiled objects"
        (src / "answer.h").write_text("#define ANSWER 42\n")
        build()
        changed = lines()[len(before):]
        assert len(changed) == 1 and "first.c" in changed[0], changed
        before = lines()
        build(flags="-O1")
        changed = lines()[len(before):]
        assert len(changed) == 3 and all(".S" not in line for line in changed), changed
        (src / "Kbuild").write_text("obj-y := first.o start.o sub/\nhostprogs-always-y := helper\n")
        build(flags="-O1")
        members = run([archive, "t", "built-in.a"], out)
        assert "second.o" not in members and "first.o" in members, members
        (out / "first.o").unlink()
        before = lines()
        build(flags="-O1")
        assert len(lines()) == len(before) + 1
        failure = run(make(out, flags="-O1 -DFAIL_COMPILE=1"), out, success=False)
        assert "requested compiler failure" in failure and "first.c" in failure, failure
        build(other, flags="-O0")
        assert (other / "built-in.a").is_file()
        assert str(out) not in (other / ".first.o.cmd").read_text()
        assert not any(src.rglob("*.o")), "Kbuild wrote into the source tree"
    print("PASS upstream provenance, native host/C/ASM Kbuild, fixdep/.cmd, no-op, headers, flags, members, repair, failure and output isolation")


if __name__ == "__main__":
    main()
