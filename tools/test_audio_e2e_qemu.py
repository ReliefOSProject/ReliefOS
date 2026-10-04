#!/usr/bin/env python3
"""Run the reproducible audio guest preflight and long Doom software gate.

The existing HDA runner owns image staging, QMP, guest probes, capture
extraction, waveform inspection, and the standard utility probe. This wrapper
adds the requested headless Doom duration and CPU/I/O load, then records the
GUI and physical-codec boundaries without substituting an older image.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/test_hda_qemu.py"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def standard_mode_output(serial_text: str, mode: str) -> str:
    """Return one standard-probe mode's serial interval, excluding neighbors."""
    start_marker = f"[u6-standard] mode={mode} "
    start = serial_text.find(start_marker)
    if start < 0:
        return ""
    end = re.search(r"\[u6-standard\] mode=[a-z]+(?:\s|$)",
                    serial_text[start + len(start_marker):])
    if not end:
        return serial_text[start:]
    return serial_text[start:start + len(start_marker) + end.start()]


def has_xrun(serial_text: str) -> bool:
    markers = bool(re.search(r"overrun!!!|underrun!!!|\bxrun\b", serial_text, re.IGNORECASE))
    recovered = any(int(count) > 0 for count in
                    re.findall(r"\bxrun_recoveries=(\d+)", serial_text))
    return markers or recovered


def doom_audio_statistics(serial_text: str) -> dict | None:
    """Sum all output lifetimes, retaining successful recovery evidence."""
    matches = re.findall(
        r"\[doom-audio\] stats written_bytes=(\d+) startup_recoveries=(\d+) "
        r"xrun_recoveries=(\d+) suspend_recoveries=(\d+)", serial_text)
    if not matches:
        return None
    fields = ("written_bytes", "startup_recoveries", "xrun_recoveries", "suspend_recoveries")
    return {"reports": len(matches), **{
        field: sum(int(values[i]) for values in matches) for i, field in enumerate(fields)
    }}


def doom_scene_statistics(serial_text: str) -> list[dict]:
    """Keep per-process music, attack and simultaneous SFX evidence."""
    fields = ("music_starts", "sfx_starts", "attack_starts", "max_sfx_voices")
    return [dict(zip(fields, map(int, values))) for values in re.findall(
        r"\[doom-audio\] scenes music_starts=(\d+) sfx_starts=(\d+) "
        r"attack_starts=(\d+) max_sfx_voices=(\d+)", serial_text)]


def doom_scenes_complete(serial_text: str) -> bool:
    """Require transitions, repeated attacks and overlapping voices in one run."""
    return any(scene["music_starts"] >= 2 and scene["sfx_starts"] >= 4 and
               scene["attack_starts"] >= 2 and scene["max_sfx_voices"] >= 2
               for scene in doom_scene_statistics(serial_text))


def doom_audio_xrun_clean(serial_text: str) -> bool:
    """Require actual writes and zero recoveries after audio begins."""
    stats = doom_audio_statistics(serial_text)
    return bool(stats and stats["written_bytes"] > 0 and
                stats["xrun_recoveries"] == 0 and stats["suspend_recoveries"] == 0 and
                not has_xrun(serial_text))


def doom_audio_throughput_clean(serial_text: str, duration: int) -> bool:
    """Require sustained 48 kHz S16 stereo output within the 2% time tolerance."""
    stats = doom_audio_statistics(serial_text)
    minimum = duration * 48000 * 4 * 98 // 100
    return bool(duration > 0 and stats and stats["written_bytes"] >= minimum)


def doom_load_complete(serial_text: str, seconds: int) -> bool:
    """Accept pressure only with the full requested audio/CPU/I/O overlap."""
    progress = doom_load_io_statistics(serial_text)
    return seconds == 0 or (
        "PASS standard-doom-load cpu-io-workers-active" in serial_text and
        bool(re.search(rf"PASS standard-doom-load-overlap seconds={seconds}(?:\s|$)",
                       serial_text)) and progress is not None and
        progress["bytes"] >= 262144 and progress["cycles"] > 0)


def doom_load_io_statistics(serial_text: str) -> dict | None:
    """Retain the bounded file worker's actual completed write counts."""
    match = re.search(r"PASS audio-io-load bytes=(\d+) cycles=(\d+)", serial_text)
    return {"bytes": int(match[1]), "cycles": int(match[2])} if match else None


def software_gate_pass(report: dict) -> bool:
    """Keep software failure strict while recording external gates separately."""
    gates = report.get("gates", {})
    required = ("alsa_guest_preflight", "soundctl_amixer_settings",
                "doom_freedoom_long_run")
    return (all(gates.get(name, False) for name in required) and
            report.get("long_run", {}).get("all_serial_xrun_clean", False))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--smp", type=int, choices=(1, 4), default=4)
    parser.add_argument("--duration", type=int, default=600,
                        help="requested Doom/pressure duration in seconds")
    parser.add_argument("--timeout", type=float, default=90,
                        help="minimum guest runner timeout; long runs extend it automatically")
    parser.add_argument("--load-seconds", type=int, default=60,
                        help="CPU/I/O load duration during the Doom run")
    parser.add_argument("--standard-probe", type=Path,
                        default=ROOT / "docs/superpowers/audio-validation/u6-standard-probe-20261003.sh")
    parser.add_argument("--probe", type=Path,
                        default=ROOT / "out/audio-hda-u6/qa/audio-guest-timer-accepted")
    args = parser.parse_args()

    image = args.image.resolve()
    output = args.output.resolve()
    if not image.is_file():
        print(f"missing image: {image}", file=sys.stderr)
        return 2
    if output == ROOT / "out" or not output.is_relative_to(ROOT / "out"):
        print("output must be a fresh worktree out subdirectory", file=sys.stderr)
        return 2
    if output.exists():
        print(f"output exists; choose a new directory: {output}", file=sys.stderr)
        return 2
    if args.duration <= 0:
        print("duration must be positive", file=sys.stderr)
        return 2
    if args.load_seconds < 0:
        print("load duration cannot be negative", file=sys.stderr)
        return 2
    standard_probe = args.standard_probe.resolve()
    if not standard_probe.is_file():
        print(f"missing standard probe: {standard_probe}", file=sys.stderr)
        return 2
    probe = args.probe.resolve()
    if not probe.is_file():
        print(f"missing guest probe: {probe}", file=sys.stderr)
        return 2

    output.parent.mkdir(parents=True, exist_ok=True)
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    invocation = [
        sys.executable, str(RUNNER),
        "--image", str(image), "--output", str(output),
        "--probe", str(probe),
        "--controller", "intel-hda", "--codec", "hda-duplex",
        "--machine", "q35", "--smp", str(args.smp), "--msi", "on",
        "--audio-backend", "alsa-file", "--timeout",
        str(max(args.timeout, args.duration + 300)),
        "--standard-probe", str(standard_probe),
        "--standard-doom-seconds", str(args.duration),
        "--standard-load-seconds", str(args.load_seconds),
    ]
    run_meta = {
        "started_utc": started,
        "image": str(image),
        "image_sha256": sha256(image),
        "duration_requested_seconds": args.duration,
        "load_requested_seconds": args.load_seconds,
        "preflight_timeout_seconds": args.timeout,
        "runner_timeout_seconds": max(args.timeout, args.duration + 300),
        "standard_probe": str(standard_probe),
        "guest_probe_binary": str(probe),
        "command": invocation,
        "long_run": {
            "status": "not_run",
            "reason": "will be updated from the standard guest serial evidence",
        },
        "physical_codec": {"status": "not_run", "reason": "ReliefOS native boot or exclusive PCI passthrough was not run"},
        "gui": {"status": "deferred", "reason": "Visible GUI validation is deferred to Xorg per user direction"},
    }
    (output.parent / (output.name + ".invocation.json")).write_text(
        json.dumps(run_meta, indent=2) + "\n"
    )
    result = subprocess.run(invocation, cwd=ROOT)
    # Preserve a machine-readable failure record even when the runner exits
    # before creating its output directory (for example, a QMP setup error).
    output.mkdir(parents=True, exist_ok=True)

    result_path = output / "result.json"
    guest = json.loads(result_path.read_text()) if result_path.is_file() else {}
    serial_text = (output / "serial.log").read_text(errors="replace") if (output / "serial.log").is_file() else ""
    standard_done = "[u6-standard] DONE failures=0" in serial_text
    doom_serial = standard_mode_output(serial_text, "doom")
    doom_pass = "PASS standard-doom headless-freedoom-level-audio" in doom_serial
    load_pass = doom_load_complete(doom_serial, args.load_seconds)
    doom_clean = ("PCM audio enabled: 48000 Hz stereo signed-16" in doom_serial and
                  "first nonzero mixed PCM block" in doom_serial and
                  "PCM submission failed" not in doom_serial and
                  "PCM space query failed" not in doom_serial and
                  "no PCM audio device" not in doom_serial)
    doom_xrun_clean = doom_audio_xrun_clean(doom_serial)
    doom_throughput_clean = doom_audio_throughput_clean(doom_serial, args.duration)
    scenes_complete = doom_scenes_complete(doom_serial)
    serial_xrun_clean = not has_xrun(serial_text)
    long_run_pass = (standard_done and doom_pass and load_pass and doom_clean and
                    doom_xrun_clean and doom_throughput_clean and scenes_complete)
    long_run = {
        "status": "passed" if long_run_pass else "failed",
        "requested_seconds": args.duration,
        "load_seconds": args.load_seconds,
        "standard_done": standard_done,
        "doom_pass": doom_pass,
        "load_pass": load_pass,
        "doom_pcm_clean": doom_clean,
        "doom_xrun_clean": doom_xrun_clean,
        "doom_throughput_clean": doom_throughput_clean,
        "doom_scenes_complete": scenes_complete,
        "doom_scene_statistics": doom_scene_statistics(doom_serial),
        "minimum_written_bytes": args.duration * 48000 * 4 * 98 // 100,
        "doom_audio_statistics": doom_audio_statistics(doom_serial),
        "doom_load_io_statistics": doom_load_io_statistics(doom_serial),
        "all_serial_xrun_clean": serial_xrun_clean,
    }
    if not long_run_pass:
        long_run["reason"] = "Doom duration, load, PCM, or in-mode XRUN evidence did not pass"
    report = {
        **run_meta,
        "long_run": long_run,
        "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "preflight_exit": result.returncode,
        "preflight": {
            "boot_complete": guest.get("boot_complete", False),
            "guest_probe": guest.get("guest_probe_pass", False),
            "hda_card_registered": guest.get("hda_card_registered", False),
            "shared_shm": guest.get("shm_guest_verified", False),
            "capture": guest.get("capture_data_verified", False),
            "waveform": guest.get("waveform_verified", False),
            "wave_error": guest.get("wave_error"),
            "capture_error": guest.get("capture_error"),
            "result_path": str(result_path),
        },
        "gates": {
            "alsa_guest_preflight": result.returncode == 0,
            "soundctl_amixer_settings": standard_done and "PASS standard-controls amixer-soundctl" in serial_text,
            "doom_freedoom_long_run": long_run_pass,
            "visible_gui_xorg": False,
            "physical_codec_matrix": False,
        },
    }
    report["software_gate_pass"] = software_gate_pass(report)
    (output / "e2e-result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, sort_keys=True))
    # GUI and physical status remain explicit; neither is a software exit gate.
    return 0 if report["software_gate_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
