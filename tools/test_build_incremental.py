#!/usr/bin/env python3
"""Exercise the production Kbuild graph in disposable outputs and kernel sources."""
from __future__ import annotations

import os
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def require(value: bool, text: str) -> None:
    if not value:
        raise AssertionError(text)
    print(f"PASS {text}", flush=True)


def archive_objects(source: Path, configuration: dict[str, str], directory: Path = Path()) -> list[str]:
    """Read enabled local builtin/composite objects in native traversal order."""
    lists: dict[str, list[str]] = {}

    def read_list(path: Path) -> None:
        for line in path.read_text().replace("\\\n", " ").splitlines():
            child = re.fullmatch(r"include \$\(src\)/([A-Za-z0-9_-]+)/Kbuild", line)
            if child:
                read_list(path.parent / child[1] / "Kbuild")
                continue
            line = re.sub(r"\$\((CONFIG_[A-Z0-9_]+)\)",
                          lambda match: configuration.get(match[1], "n"), line)
            match = re.match(r"^([A-Za-z0-9_.-]+)\s*([:+]?=)\s*(.*?)\s*(?:#.*)?$", line)
            if not match:
                continue
            key, operation, value = match.groups()
            prefix = re.fullmatch(r"\$\(addprefix ([A-Za-z0-9_-]+/),\$\(([A-Za-z0-9_.-]+)\)\)", value)
            entries = ([prefix[1] + item for item in lists[prefix[2]]]
                       if prefix else value.split())
            if operation in ("=", ":="):
                lists[key] = []
            lists.setdefault(key, []).extend(entries)

    read_list(source / directory / "Kbuild")
    result: list[str] = []

    def append_object(entry: str) -> None:
        # Native builtin archives flatten composite members; the same list
        # becomes a relocatable composite when selected as a module.
        members = lists.get(entry[:-2] + "-y", lists.get(entry[:-2] + "-objs"))
        if members is not None:
            for member in members:
                append_object(member)
        else:
            # Generated sources such as vdso_blob.S also belong to the local
            # explicit list; the kernel's ownership audit checks their rules.
            if not entry.endswith(".o") or any(part in ("", ".", "..") for part in entry.split("/")):
                raise AssertionError(f"invalid native member: {directory}/{entry}")
            result.append((directory / entry).as_posix())

    for entry in lists.get("obj-y", []):
        if entry.endswith("/") and "/" not in entry[:-1]:
            result.extend(archive_objects(source, configuration, directory / entry))
        elif entry.endswith(".o") and "/" not in entry:
            append_object(entry)
        else:
            raise AssertionError(f"non-local Kbuild member: {directory}/{entry}")
    return result



def check_runtime_elf(output: Path) -> None:
    for name in ("libreliefos.so.2", "libleonos.so.2"):
        dynamic = subprocess.check_output(["readelf", "-d", str(output / "system/lib" / name)], text=True)
        match = re.search(r"\(SONAME\).*\[([^\]]+)\]", dynamic)
        require(match is not None and match[1] == name,
                f"runtime {name} preserves its distinct SONAME")


def main() -> None:
    temporary = Path(os.environ.get("TMPDIR", ROOT / "out/tmp"))
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kbuild-incremental-", dir=temporary) as name:
        work = Path(name)
        source = work / "kernel"
        child = ROOT / "kernel/reliefnt"
        def ignored(directory: str, names: list[str]) -> set[str]:
            # Only the source root's cache is an artifact; mm/cache is code.
            excluded = {"__pycache__"}
            if Path(directory) == child:
                excluded.update((".git", "out", "cache"))
            return set(names) & excluded

        shutil.copytree(child, source, ignore=ignored)
        shutil.copytree(child / "cache", source / "cache")
        out = work / "output"
        kernel = out / "reliefnt"
        wrapper = work / "archiver"
        wrapper.write_text('#!/bin/sh\nexec llvm-ar "$@"\n')
        wrapper.chmod(0o755)
        arguments = ["make", "--no-print-directory", "-C", str(ROOT), f"O={out}",
                     f"RELIEFNT_DIR={source}", f"AR={wrapper}", "SOURCE_DATE_EPOCH=1700000000", "-j6"]
        step = 0
        logs = ROOT / "out/kbuild-migration" / work.name
        logs.mkdir(parents=True, exist_ok=True)
        print(f"build logs: {logs}", flush=True)

        def build(*extra: str) -> str:
            nonlocal step
            step += 1
            result = subprocess.run(arguments + list(extra) + ["kernel", "runtime"],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (logs / f"step-{step}.log").write_text(result.stdout)
            if result.returncode:
                raise AssertionError(f"build {step} failed ({result.returncode}):\n" + "\n".join(result.stdout.splitlines()[-35:]))
            return result.stdout

        def objects() -> dict[Path, tuple[Path, int]]:
            result = {}
            for record in out.rglob(".*.o.cmd"):
                match = re.search(r"^source_[^\n]* := (.+)$", record.read_text(), re.M)
                if not match:
                    continue
                file = Path(match[1])
                # First-party native source prerequisites are absolute; generated
                # vDSO assembly is relative to the native kernel output cwd.
                if not file.is_absolute():
                    file = kernel / file
                file = file.resolve()
                if not (file.is_relative_to(source) or file.is_relative_to(ROOT / "userland")):
                    continue
                obj = record.with_name(record.name[1:-4])
                if obj.is_file():
                    result[obj] = (file, obj.stat().st_mtime_ns)
            return result

        def changed(before: dict, after: dict) -> set[Path]:
            return {path for path in before.keys() & after.keys() if before[path][1] != after[path][1]}

        def runtime_only(snapshot: dict) -> dict:
            return {obj: data for obj, data in snapshot.items()
                    if data[0].is_relative_to(ROOT / "userland")}

        build()
        check_runtime_elf(out)
        baseline = objects()
        require(len(baseline) > 200, "production kernel and runtime first-party objects built")
        members = subprocess.check_output(["llvm-ar", "t", str(kernel / "built-in.a")], text=True).splitlines()
        members = [str(Path(path).resolve().relative_to(kernel)) for path in members]
        configuration = dict(line.split("=", 1) for line in (kernel / ".config").read_text().splitlines()
                             if line.startswith("CONFIG_") and "=" in line)
        expected = archive_objects(source, configuration)
        require(members == expected, "archive contains exactly the intended source objects in order")
        output_before = (out / "generated/system/kernel.sys").read_bytes()
        build()
        noop = objects()
        if baseline != noop:
            report = {"added": [str(p) for p in noop.keys() - baseline.keys()],
                      "removed": [str(p) for p in baseline.keys() - noop.keys()],
                      "changed": {str(p): {"before": [str(baseline[p][0]), baseline[p][1]],
                                           "after": [str(noop[p][0]), noop[p][1]]}
                                  for p in baseline.keys() & noop.keys() if baseline[p] != noop[p]}}
            (logs / "noop-difference.json").write_text(json.dumps(report, indent=2))
        require(baseline == noop, "no-op leaves all native object mtimes unchanged")

        file = source / "kernel/ipc/futex.c"
        file.touch()
        build()
        current = objects()
        moved = changed(baseline, current)
        require(len(moved) == 1 and current[next(iter(moved))][0] == file,
                "one kernel C source rebuilds exactly its object")
        require(runtime_only(baseline) == runtime_only(current), "kernel source change rebuilds no runtime object")
        require(output_before == (out / "generated/system/kernel.sys").read_bytes(),
                "content-preserving source touch leaves published kernel bytes unchanged")

        before = current
        (source / "include/reliefnt/sched.h").touch()
        build()
        current = objects()
        require(len(changed(before, current)) > 1, "private kernel header rebuilds its kernel consumers")
        require(runtime_only(before) == runtime_only(current), "private kernel header rebuilds no runtime object")

        before = current
        public = source / "include/uapi/reliefos/fs_abi.h"
        public_before = public.read_bytes()
        public.write_bytes(public_before + b"\n/* isolated Kbuild UAPI dependency probe */\n")
        build()
        current = objects()
        require(bool(changed(runtime_only(before), runtime_only(current))),
                "exported UAPI content change rebuilds runtime consumers")
        require((out / "kernel-export/include/reliefos/fs_abi.h").read_bytes() == public.read_bytes(),
                "parent header publication has the changed UAPI bytes")
        public.write_bytes(public_before)
        build()
        current = objects()
        require(output_before == (out / "generated/system/kernel.sys").read_bytes(),
                "restoring UAPI source restores its exact native ABI and kernel bytes")

        before = current
        build("KERNEL_CFLAGS=-DRELIEFNT_KBUILD_FLAG_PROBE=1")
        current = objects()
        original_asm = {obj for obj, (file, _) in before.items()
                        if file.is_relative_to(source / "arch") and file.suffix == ".S"}
        require(not (changed(before, current) & original_asm), "C flag change leaves original assembly objects alone")
        require(len(changed(before, current)) >= 157, "C flag change rebuilds the kernel C objects")
        require(runtime_only(before) == runtime_only(current), "kernel C flags do not change runtime objects")
        build()

        before = objects()
        linked = kernel / "generated/system/kernel.unstripped"
        link_time = linked.stat().st_mtime_ns
        (source / "arch/x86_64/linker.ld").touch()
        build()
        require(before == objects() and linked.stat().st_mtime_ns != link_time,
                "linker script change relinks without recompiling")

        before = objects()
        archive_time = (kernel / "built-in.a").stat().st_mtime_ns
        wrapper_before = wrapper.read_bytes()
        release_before = (kernel / "include/config/kernel.release").read_bytes()
        abi_consumers = {obj for obj in before if "include/generated/module_abi.h" in
                         obj.with_name(f".{obj.name}.cmd").read_text()}
        wrapper.write_bytes(wrapper_before + b"# in-place tool identity change\n")
        build()
        require((kernel / "built-in.a").stat().st_mtime_ns != archive_time,
                "archiver replacement at the same path rebuilds the archive immediately")
        current = objects()
        require(release_before != (kernel / "include/config/kernel.release").read_bytes(),
                "changed archiver bytes invalidate the conservative native module ABI")
        require(changed(before, current) <= abi_consumers and runtime_only(before) == runtime_only(current),
                "archiver replacement recompiles only actual ABI-header consumers")
        build()
        require(current == objects(), "tool identity invalidation settles to no-op")
        wrapper.write_bytes(wrapper_before)
        build()
        require(output_before == (out / "generated/system/kernel.sys").read_bytes(),
                "restoring identical tool bytes restores the original native ABI and kernel")

        before = objects()
        victim = next(obj for obj, (file, _) in before.items() if file == source / "kernel/ipc/futex.c")
        victim.unlink()
        for path in (out / "generated/system/kernel.sys", out / "system/lib/libreliefos.so.2",
                     out / "musl/lib/libreliefos.a"):
            path.unlink()
        build()
        current = objects()
        require(victim.is_file() and changed(before, current) == {victim},
                "missing object and products are repaired without unrelated compilation")
        require(output_before == (out / "generated/system/kernel.sys").read_bytes(), "repaired kernel has original bytes")
        require((out / "system/lib/libreliefos.so.2").is_file() and (out / "musl/lib/libreliefos.a").is_file(),
                "both shared and static runtime products are repaired")
        before = current
        exported = out / "kernel-export/include/reliefos/fs_abi.h"
        header_consumers = {obj for obj in runtime_only(before)
                            if "kernel-export/include/reliefos/fs_abi.h" in
                            obj.with_name(f".{obj.name}.cmd").read_text()}
        require(bool(header_consumers), "native records identify exported header consumers")
        exported.unlink()
        build()
        current = objects()
        require(exported.read_bytes() == public.read_bytes(), "missing exported header is repaired")
        require(changed(before, current) == header_consumers,
                "restored exported header rebuilds exactly its native consumers")
        require(not list(source.rglob("*.o")), "source fixture contains no generated object")

        # Exercise standalone asset and clean entry points, not only parent
        # integration. Unknown files and the user's selected config survive.
        fresh = work / "build-info"
        result = subprocess.run(["make", "--no-print-directory", "-C", str(source),
                                 f"O={fresh}", "-j6", "build-info"],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (logs / "standalone-build-info.log").write_text(result.stdout)
        require(result.returncode == 0 and (fresh / "include/generated/build_info.h").is_file(),
                "standalone build-info builds its host prerequisite from a fresh output")
        sentinel = kernel / "user.keep"
        sentinel.write_text("unowned output entry\n")
        config = (kernel / ".config").read_bytes()
        result = subprocess.run(["make", "--no-print-directory", "-C", str(source),
                                 f"O={kernel}", "clean"],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (logs / "standalone-clean.log").write_text(result.stdout)
        require(result.returncode == 0 and not list(kernel.rglob("*.o")) and
                not (kernel / "lib").exists() and not (kernel / "sound").exists(),
                "clean removes all kernel objects including lib and sound")
        require(sentinel.read_text() == "unowned output entry\n" and
                (kernel / ".config").read_bytes() == config,
                "clean preserves unknown entries and configuration")
        build()
        require(output_before == (out / "generated/system/kernel.sys").read_bytes(),
                "clean release rebuild reproduces the original published kernel")
        before = objects()
        linked_time = (kernel / "generated/system/kernel.unstripped").stat().st_mtime_ns
        build()
        require(before == objects() and
                linked_time == (kernel / "generated/system/kernel.unstripped").stat().st_mtime_ns,
                "no-op after clean rebuild neither recompiles nor relinks")


if __name__ == "__main__":
    main()
