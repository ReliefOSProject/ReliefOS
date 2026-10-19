#!/usr/bin/env python3
"""Compile Linux-6.12 DRM ABI and production-core fixtures with sanitizers."""
from pathlib import Path
import subprocess
import os
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INCLUDES = [
    "-Ikernel/reliefnt/include",
    "-Ikernel/reliefnt/include/uapi",
    "-Ikernel/reliefnt/kernel/reliefnt/include",
    "-Iinclude",
]
FIXTURES = [
    ("tools/tests/drm_abi_test.c", "drm-abi"),
    ("tools/tests/drm_execbuf_test.c", "drm-execbuf"),
    ("tools/tests/drm_core_test.c", "drm-core"),
    ("tools/tests/drm_vma_clear_test.c", "drm-vma-clear"),
    ("tools/tests/framebuffer_drm_test.c", "framebuffer-drm"),
    ("tools/tests/console_replay_test.c", "console-replay"),
]


def main() -> int:
    temp_parent = ROOT / "out/tmp"
    temp_parent.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temp_parent)
    with tempfile.TemporaryDirectory(prefix="reliefos-drm-", dir=temp_parent) as directory:
        generated_include = Path(directory) / "include"
        generated_font = generated_include / "generated" / "cjk_font.h"
        generated_font.parent.mkdir(parents=True)
        generated_font.write_text(
            "static const struct { uint32_t cp; uint8_t width; uint8_t bits[32]; } "
            "cjk_glyphs[] = {{0xfffd, 1, {0}}};\n",
            encoding="utf-8",
        )
        for source, name in FIXTURES:
            binary = Path(directory) / name
            command = [
                "clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra",
                "-Werror", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                *INCLUDES, source, "-Wl,--gc-sections", "-o", str(binary),
            ]
            if name in ("framebuffer-drm", "console-replay"):
                command[1:1] = [f"-I{generated_include}",
                                "-Ikernel/reliefnt/drivers/video/framebuffer"]
            print("$ " + " ".join(command))
            subprocess.run(command, cwd=ROOT, check=True)
            if name == "console-replay":
                shadow_output = Path(directory) / "console-replay-shadow.bin"
                fallback_output = Path(directory) / "console-replay-fallback.bin"
                runs = [
                    ([str(binary), str(shadow_output), "shadow"], "shadow"),
                    ([str(binary), str(fallback_output), "fallback"], "fallback"),
                ]
                for arguments, mode in runs:
                    result = subprocess.run(arguments, cwd=ROOT, capture_output=True,
                                            text=True, timeout=60)
                    print(f"[{mode}] {result.stdout}", end="")
                    if result.returncode:
                        print(result.stderr, end="")
                        return result.returncode
                if shadow_output.read_bytes() != fallback_output.read_bytes():
                    print("console-replay: shadow and allocation-failure fallback differ")
                    return 1
                reference_output = ROOT / "out/task6c-console-reference/reference.bin"
                if reference_output.is_file():
                    reference = reference_output.read_bytes()
                    if shadow_output.read_bytes() != reference:
                        print("console-replay: shadow output differs from frozen direct-render reference")
                        return 1
                    if fallback_output.read_bytes() != reference:
                        print("console-replay: fallback output differs from frozen direct-render reference")
                        return 1
                    print("console-replay: both paths match frozen direct-render reference")
                print("console-replay: shadow and fallback outputs match")
            else:
                result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True,
                                        text=True, timeout=30)
                print(result.stdout, end="")
                if result.returncode:
                    print(result.stderr, end="")
                    return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
