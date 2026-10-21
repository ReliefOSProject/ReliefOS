#!/usr/bin/env python3
"""Build and exercise the production kernel memcpy/memset on the host."""

from pathlib import Path
import os
import platform
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TMP_ROOT = ROOT / "out/tmp"
PRODUCTION = ROOT / "kernel/reliefnt/lib/string.c"
HARNESS = ROOT / "tools/tests/kernel_memory_test.c"


def main() -> int:
    if platform.machine().lower() not in {"x86_64", "amd64"}:
        raise SystemExit("kernel memory test requires an x86_64 host")

    TMP_ROOT.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["TMPDIR"] = str(TMP_ROOT)

    cc = shutil.which("cc")
    if cc is None:
        raise SystemExit("C compiler 'cc' was not found")

    compilers = [("cc", cc)]
    clang = shutil.which("clang")
    if clang is not None and clang != cc:
        compilers.append(("clang", clang))

    compile_flags = [
        "-std=c11",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fno-builtin",
        "-fno-lto",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer",
    ]

    with tempfile.TemporaryDirectory(
        prefix="reliefos-kernel-memory-", dir=TMP_ROOT
    ) as temporary:
        work = Path(temporary)
        for compiler_index, (compiler_name, compiler) in enumerate(compilers):
            compiler_tag = f"{compiler_index}_{compiler_name}"
            harness_object = work / f"kernel_memory_harness_{compiler_tag}.o"
            subprocess.run(
                [
                    compiler,
                    *compile_flags,
                    "-O1",
                    "-c",
                    str(HARNESS),
                    "-o",
                    str(harness_object),
                ],
                cwd=ROOT,
                env=env,
                check=True,
            )

            for optimization in ("0", "3"):
                production_object = work / (
                    f"kernel_string_{compiler_tag}_O{optimization}.o"
                )
                binary = work / f"kernel_memory_{compiler_tag}_O{optimization}"
                print(
                    f"== {compiler_name} production string.c -O{optimization} ==",
                    flush=True,
                )
                subprocess.run(
                    [
                        compiler,
                        *compile_flags,
                        f"-O{optimization}",
                        "-Ikernel/reliefnt/include",
                        "-Dmemcpy=kernel_memcpy",
                        "-Dmemset=kernel_memset",
                        "-Dmemcmp=kernel_memcmp",
                        "-Dstrlen=kernel_strlen",
                        "-Dstrcmp=kernel_strcmp",
                        "-Dstrncmp=kernel_strncmp",
                        "-c",
                        str(PRODUCTION),
                        "-o",
                        str(production_object),
                    ],
                    cwd=ROOT,
                    env=env,
                    check=True,
                )
                subprocess.run(
                    [
                        compiler,
                        *compile_flags,
                        str(harness_object),
                        str(production_object),
                        "-o",
                        str(binary),
                    ],
                    cwd=ROOT,
                    env=env,
                    check=True,
                )
                subprocess.run(
                    [str(binary)], cwd=ROOT, env=env, check=True, timeout=60
                )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
