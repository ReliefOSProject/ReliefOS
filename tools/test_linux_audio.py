#!/usr/bin/env python3
"""Validate the pinned Linux v6.14 audio UAPI layout and ioctl contract."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tools/tests/audio_abi_layout_test.c"
PCM_PROBE = ROOT / "tools/tests/audio_pcm_state_test.c"
VMA_PROBE = ROOT / "tools/tests/audio_vma_test.c"
DEVICE_PROBE = ROOT / "tools/tests/audio_device_test.c"
PARAMS_PROBE = ROOT / "tools/tests/audio_alsa_params_test.c"
IOCTL_PROBE = ROOT / "tools/tests/audio_alsa_ioctl_test.c"
MMAP_PROBE = ROOT / "tools/tests/audio_mmap_test.c"
CONTROL_PROBE = ROOT / "tools/tests/audio_control_test.c"
OSS_PROBE = ROOT / "tools/tests/audio_oss_multichannel_test.c"
OSS_WAIT_PROBE = ROOT / "tools/tests/audio_oss_wait_test.c"
MIXER_PROBE = ROOT / "tools/tests/audio_mixer_test.c"
GUEST_PROBE = ROOT / "tools/tests/audio_guest_test.c"
FIXTURE = ROOT / "tools/tests/fixtures/audio-uapi/linux-v6.14"
PINNED = {
    FIXTURE / "sound/asound.h": "ec647abccd554aab1ebbf77af42efac11748384f027870eb90cba974b6bcda0a",
    FIXTURE / "sound/tlv.h": "74b97d2cae70e67fd7bc5750dd46e0b9976d390fb817d98630df6ba9c2d33d61",
    FIXTURE / "linux/soundcard.h": "dd8de1b851b7b6115edb33b1d7f03f8887c05a3dcbac637a4784220a6faeaaa4",
}


def enum_names() -> list[str]:
    names: set[str] = set()
    for header in (ROOT / "kernel/reliefnt/include/uapi/sound/asound.h",
                   ROOT / "kernel/reliefnt/include/uapi/linux/soundcard.h"):
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", header.read_text(), flags=re.S)
        for match in re.finditer(r"\benum(?:\s+[A-Za-z_]\w*)?\s*\{", source):
            depth = 1
            end = match.end()
            while end < len(source) and depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            body = source[match.end():end - 1]
            for item in body.split(","):
                name = item.split("=", 1)[0].strip()
                found = re.fullmatch(r"([A-Za-z_]\w*)", name)
                if found:
                    names.add(found.group(1))
    return sorted(names)


def extra_macro_names() -> list[str]:
    names: set[str] = set()
    headers = (ROOT / "kernel/reliefnt/include/uapi/sound/asound.h",
               ROOT / "kernel/reliefnt/include/uapi/linux/soundcard.h")
    for header in headers:
        for line in header.read_text().splitlines():
            match = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)(\([^)]*\))?\s*(.*)", line)
            if not match or match.group(2):
                continue
            name, replacement = match.group(1), match.group(3).strip()
            selected = (name.startswith(("SNDRV_PCM_IOCTL_", "SNDRV_CTL_IOCTL_",
                                         "SNDCTL_", "SOUND_")))
            if selected and replacement and not replacement.startswith(("{", '"')):
                names.add(name)
    return sorted(names)


def wrapper_source() -> str:
    values = [f'    printf("value.{name}=%lu\\n", (unsigned long)({name}));'
              for name in enum_names() + extra_macro_names()]
    return ("#define AUDIO_ABI_NO_MAIN\n"
            f'#include "{PROBE}"\n'
            "int main(void) {\n"
            "    print_layout();\n"
            + "\n".join(values) + "\n"
            "    return 0;\n"
            "}\n")


def run_probe(include_dir: Path, source: Path, binary: Path, *, fixture: bool = False,
              time64: bool = False) -> str:
    command = [
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
    ]
    if fixture:
        command.extend([
            "-D__user=",
            "-D__force=",
            "-D__packed=__attribute__((packed))",
        ])
    if time64:
        command.append("-D__SND_STRUCT_TIME64")
    command.extend(["-I", str(include_dir), str(source), "-o", str(binary)])
    subprocess.run(command, cwd=ROOT, check=True, timeout=20)
    result = subprocess.run([str(binary)], cwd=ROOT, check=True, text=True,
                            capture_output=True, timeout=10)
    return result.stdout


def run_pcm() -> int:
    if not PCM_PROBE.is_file():
        print(f"missing probe: {PCM_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-pcm-") as directory:
        binary = Path(directory) / "pcm"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(PCM_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS PCM state test")
    return 0


def run_device() -> int:
    if not DEVICE_PROBE.is_file():
        print(f"missing probe: {DEVICE_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-device-") as directory:
        binary = Path(directory) / "device"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(DEVICE_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS audio device node/OFD/read-write/poll")
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-devfs-") as directory:
        binary = Path(directory) / "devfs"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            "tools/tests/audio_devfs_test.c", "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    return 0


def run_vma() -> int:
    if not VMA_PROBE.is_file():
        print(f"missing probe: {VMA_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-vma-") as directory:
        binary = Path(directory) / "device"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(VMA_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS audio VMA lifetime")
    return 0


def run_params() -> int:
    if not PARAMS_PROBE.is_file():
        print(f"missing probe: {PARAMS_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-params-") as directory:
        binary = Path(directory) / "params"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(PARAMS_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS ALSA PCM parameter refine")
    return 0


def run_ioctl() -> int:
    if not IOCTL_PROBE.is_file():
        print(f"missing probe: {IOCTL_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-ioctl-") as directory:
        binary = Path(directory) / "ioctl"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(IOCTL_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS ALSA PCM ioctl protocol/state")
    return 0


def run_mmap() -> int:
    if not MMAP_PROBE.is_file():
        print(f"missing probe: {MMAP_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-mmap-") as directory:
        binary = Path(directory) / "mmap"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(MMAP_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS ALSA PCM mmap contract")
    return 0


def run_control() -> int:
    if not CONTROL_PROBE.is_file():
        print(f"missing probe: {CONTROL_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-control-") as directory:
        binary = Path(directory) / "control"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(CONTROL_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS ALSA control/event/TLV")
    return 0


def run_oss() -> int:
    if not OSS_PROBE.is_file():
        print(f"missing probe: {OSS_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-oss-") as directory:
        binary = Path(directory) / "oss"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(OSS_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
        wait_binary = Path(directory) / "oss_wait"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(OSS_WAIT_PROBE), "-o", str(wait_binary),
        ], cwd=ROOT, check=True, timeout=20)
        failures = []
        for direction in ("read", "write", "trigger", "prefill", "full-prefill", "grow-buffer", "bulk", "fragment", "reset", "tail", "full-tail", "negotiate", "lease", "xrun", "u8-sparse", "odd-channels", "boundary"):
            result = subprocess.run([str(wait_binary), direction], cwd=ROOT, timeout=10)
            if result.returncode:
                failures.append((direction, result.returncode))
        if failures:
            raise RuntimeError(f"OSS wait fixture failures: {failures}")
    print("PASS OSS control/queue/fragment and real interruptible waits")
    return 0


def run_mixer() -> int:
    if not MIXER_PROBE.is_file():
        print(f"missing probe: {MIXER_PROBE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="reliefos-audio-mixer-") as directory:
        binary = Path(directory) / "mixer"
        subprocess.run([
            "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-no-pie", "-pthread",
            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
            "-Wl,--gc-sections", "-Ikernel/reliefnt/include",
            "-Ikernel/reliefnt/include/uapi", "-Ikernel/reliefnt/kernel/reliefnt/include",
            str(MIXER_PROBE), "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=20)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=10)
    print("PASS OSS mixer/shared ALSA control")
    return 0


def run_abi() -> int:
    if not PROBE.is_file():
        print(f"missing probe: {PROBE}", file=sys.stderr)
        return 2

    for path, expected in PINNED.items():
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f"fixture hash mismatch: {path}: {actual}")

    with tempfile.TemporaryDirectory(prefix="reliefos-audio-abi-") as directory:
        work = Path(directory)
        wrapper = work / "probe.c"
        wrapper.write_text(wrapper_source())
        for time64 in (False, True):
            suffix = "time64" if time64 else "native"
            production = run_probe(ROOT / "kernel/reliefnt/include/uapi", wrapper,
                                    work / f"production-{suffix}", time64=time64)
            reference = run_probe(FIXTURE, wrapper, work / f"reference-{suffix}",
                                  fixture=True, time64=time64)
            if production != reference:
                print(f"ABI probe mismatch ({suffix}) between production and Linux v6.14 fixture:",
                      file=sys.stderr)
                import difflib
                sys.stderr.writelines(difflib.unified_diff(
                    reference.splitlines(keepends=True), production.splitlines(keepends=True),
                    fromfile=f"linux-v6.14-{suffix}", tofile=f"reliefos-{suffix}"))
                return 1
    print(f"PASS Linux v6.14 audio UAPI layouts, {len(enum_names())} enum values, "
          f"{len(extra_macro_names())} ioctl/OSS constants (native and time64)")
    return 0


def build_guest(output: Path, sdk: Path, period_frames: int = 512,
                buffer_frames: int = 2048) -> int:
    """Compile one source against Linux reference, production UAPI and musl.

    Run only ABI and invalid arguments on the host. Real sound operations are
    deferred to H6/U6; an available host microphone is never a prerequisite.
    """
    output = output.resolve()
    if not output.is_relative_to(ROOT / "out"):
        raise SystemExit("guest output must be in this worktree's build directory")
    if period_frames < 32 or period_frames % 32:
        raise SystemExit("guest period must be a multiple of 32 frames and at least 32")
    if buffer_frames < period_frames * 2 or buffer_frames % period_frames:
        raise SystemExit("guest buffer must contain an integral number of at least two periods")
    if buffer_frames > 16384:
        raise SystemExit("guest buffer exceeds the 64 KiB S16 stereo PCM limit")
    output.parent.mkdir(parents=True, exist_ok=True)
    for path, digest in PINNED.items():
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise SystemExit(f"fixture hash mismatch: {path}")
    common = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
              f"-DAUDIO_GUEST_PERIOD_FRAMES={period_frames}",
              f"-DAUDIO_GUEST_BUFFER_FRAMES={buffer_frames}"]
    host, reference = Path(str(output) + "-host"), Path(str(output) + "-linux-v6.14")
    commands = [
        ["cc", *common, "-I", str(ROOT / "kernel/reliefnt/include/uapi"),
         str(GUEST_PROBE), "-lm", "-o", str(host)],
        ["cc", *common, "-I", str(FIXTURE), "-D__user=", "-D__force=",
         "-D__packed=__attribute__((packed))", str(GUEST_PROBE), "-lm", "-o", str(reference)],
        [str(sdk.resolve() / "bin/reliefos-musl-cc"), *common, "-static",
         str(GUEST_PROBE), "-lm", "-o", str(output)],
    ]
    for command in commands:
        subprocess.run(command, cwd=ROOT, check=True, timeout=60)
    reference_layout = subprocess.check_output([str(reference), "--mode", "abi"], text=True, timeout=10)
    for binary in (host, output):
        layout = subprocess.check_output([str(binary), "--mode", "abi"], text=True, timeout=10)
        if layout != reference_layout:
            raise SystemExit(f"guest ABI differs from pinned Linux v6.14: {binary}")
        for invalid in (["--mode", "invalid"], ["--card", "-1", "--mode", "abi"],
                        ["--mode", "capture", "--frames", "0"], ["--mode"]):
            result = subprocess.run([str(binary), *invalid], capture_output=True, timeout=10)
            if result.returncode != 2:
                raise SystemExit(f"invalid arguments not rejected: {binary}: {invalid}")
    Path(str(output) + "-linux-v6.14-abi.txt").write_text(reference_layout)
    fixture = Path(str(output) + "-transport-test")
    subprocess.run(["cc", *common, "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-I", str(ROOT / "kernel/reliefnt/include/uapi"),
                    str(ROOT / "tools/tests/audio_guest_transport_test.c"),
                    "-lm", "-o", str(fixture)], cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(fixture)], cwd=ROOT, check=True, timeout=10)
    # Both include orders must compile against the actual SDK, not host /usr/include.
    for first in ("time.h", "sound/asound.h"):
        second = "sound/asound.h" if first == "time.h" else "time.h"
        source = f"#include <{first}>\n#include <{second}>\n_Static_assert(sizeof(struct timespec)==16, \"native time64\");\n"
        include_obj = output.parent / ("audio-guest-include-" + first.replace("/", "-"))
        subprocess.run([str(sdk.resolve() / "bin/reliefos-musl-cc"), *common,
                        "-Wno-unused-command-line-argument", "-x", "c", "-c", "-o", str(include_obj), "-"], input=source,
                       text=True, cwd=ROOT, check=True, timeout=20)
    binaries = [host, reference, output, fixture]
    Path(str(output) + "-build.json").write_text(json.dumps({
        "source_sha256": hashlib.sha256(GUEST_PROBE.read_bytes()).hexdigest(),
        "pcm_geometry": {"period_frames": period_frames,
                         "buffer_frames": buffer_frames,
                         "buffer_bytes": buffer_frames * 4},
        "binaries": [{"path": str(p.relative_to(ROOT)),
                      "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in binaries],
        "evidence": "host ABI/invalid arguments/transport fixture and musl build only",
        "hardware_io_run": False, "guest_run": False,
    }, indent=2) + "\n")
    print(f"PASS guest probe: pinned Linux/production/musl ABI, invalid arguments, transport, include orders; built {output}")
    return 0


def main() -> int:
    modes = {"abi": run_abi, "pcm": run_pcm, "vma": run_vma,
             "device": run_device, "params": run_params, "ioctl": run_ioctl,
             "mmap": run_mmap, "control": run_control, "oss": run_oss, "mixer": run_mixer}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modes", nargs="*", metavar="mode")
    parser.add_argument("--build-guest", action="store_true")
    parser.add_argument("--guest-period-frames", type=int, default=512)
    parser.add_argument("--guest-buffer-frames", type=int, default=2048)
    parser.add_argument("--output", type=Path, default=ROOT / "out/audio-hda/qa/audio-guest")
    parser.add_argument("--sdk", type=Path, default=ROOT / "out/audio-hda/sdk/reliefos-musl-sdk")
    args = parser.parse_args()
    if not args.modes and not args.build_guest:
        parser.error("select one or more modes or --build-guest")
    for mode in args.modes:
        if mode not in modes:
            parser.error(f"unknown mode {mode}; choose from {', '.join(modes)}")
    for mode in args.modes:
        result = modes[mode]()
        if result:
            return result
    return build_guest(args.output, args.sdk, args.guest_period_frames,
                       args.guest_buffer_frames) if args.build_guest else 0


if __name__ == "__main__":
    raise SystemExit(main())
