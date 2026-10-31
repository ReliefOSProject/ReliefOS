#!/usr/bin/env python3
"""Exercise shipped IceWM with real pointer/key input on a private QEMU disk.

Requires the Xorg test image's test/test account and default st window. This checks
completed interactions and release handling; screenshot timing is not an FPS
measurement. Use --kernel to test an incremental kernel without rebuilding Xorg.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

from run_gcc_probe_qemu import qmp_quit
from test_installer_accounts_qemu import Probe
from test_vdso_qemu import file_sha256, gpt_partitions, install_private_kernel
from test_xorg_vm_qemu import boot, greeter_login, session_st_bounds, st_marker, wait_session

ROOT = Path(__file__).resolve().parents[1]


def client_bounds(frame):
    return session_st_bounds(frame)


def wait_bounds(probe, process, name, predicate, timeout=3):
    started = time.monotonic()
    last = None
    stable = 0
    while time.monotonic() - started < timeout:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited during IceWM interaction")
        current = client_bounds(probe.frame(name))
        stable = stable + 1 if current == last and predicate(current) else 0
        last = current
        if stable >= 1:
            return last, time.monotonic() - started
        time.sleep(.025)
    raise AssertionError(f"{name}: client geometry did not converge: {last}")


def modifier(probe, key, down):
    probe.command("input-send-event", {"events": [
        {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}]})


def drag(probe, process, name, before, dx, dy, *, alt=False, resize=False,
         motion_interval=.025):
    x = before[2] + 2 if resize else before[0] + 180
    y = before[3] + 2 if resize else before[1] + 80 if alt else before[1] - 10
    probe.move(x, y)
    time.sleep(.2)
    if alt:
        modifier(probe, "alt", True)
    probe.button(True)
    time.sleep(.1)
    try:
        for step in range(1, 41):
            probe.move(x + round(dx * step / 40), y + round(dy * step / 40))
            time.sleep(motion_interval)
    finally:
        probe.button(False)
        if alt:
            modifier(probe, "alt", False)
    if resize:
        expected = (before[0], before[1], before[2] + dx, before[3] + dy)
        tolerance = 20  # st resizes in character cells.
    else:
        expected = tuple(v + (dx if i % 2 == 0 else dy) for i, v in enumerate(before))
        tolerance = 6
    after, elapsed = wait_bounds(probe, process, name, lambda b: b is not None and
                                all(abs(a - e) <= tolerance for a, e in zip(b, expected)))
    # Pointer movement after button release must not keep moving/resizing.
    probe.move(after[0] + 250, after[1] + 110)
    time.sleep(.2)
    released = client_bounds(probe.frame(name + "-released"))
    assert released is not None and all(abs(a - b) <= tolerance for a, b in zip(released, after)), \
        f"{name}: pointer still grabbed after release: {after} -> {released}"
    return released, {"name": name, "before": before, "after": released,
                   "post_release_screenshot_seconds": elapsed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "out/xorg-vmware-debug/images/reliefos.raw")
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--motion-interval", type=float, default=.025,
                        help="seconds between QMP pointer samples (default: 40 Hz)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.motion_interval <= 0:
        parser.error("--motion-interval must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = args.image.resolve()
    source_hash = file_sha256(source)
    raw = output / "guest.raw"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", source, raw], check=True)
    metadata = {"source_image": str(source), "source_sha256": source_hash,
                "motion_interval": args.motion_interval, "checks": []}
    if args.kernel:
        esp = next(p for p in gpt_partitions(raw) if p["name"] == "RELIEFOS_ESP")
        metadata["kernel"] = install_private_kernel(raw, int(esp["first"]), args.kernel.resolve(), output)
    raw_hash = file_sha256(raw)
    process, serial, qmp = boot([], raw, output)
    probe = None
    try:
        deadline = time.monotonic() + 20
        while not qmp.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU failed to start")
            time.sleep(.1)
        probe = Probe(qmp, output)
        greeter_login(probe, process)
        wait_session(probe, process, "session", timeout=90)
        st_marker(probe, serial, process, "ICEWM-INTERACTIONS-BEGIN")
        bounds = client_bounds(probe.frame("initial"))
        assert bounds is not None and bounds[2] - bounds[0] > 400
        for name, dx, dy, alt in (("title-drag", 120, 60, False),
                                  ("alt-drag", 100, 40, True),
                                  ("title-drag-back", -100, -40, False)):
            bounds, result = drag(probe, process, name, bounds, dx, dy, alt=alt,
                                  motion_interval=args.motion_interval)
            metadata["checks"].append(result)
        bounds, result = drag(probe, process, "resize", bounds, 80, 48, resize=True,
                              motion_interval=args.motion_interval)
        metadata["checks"].append(result)
        normal = bounds
        # Match KeyWinMaximize/KeyWinMinimize in the shipped preferences.
        probe.key("meta_l-z")
        bounds, _ = wait_bounds(probe, process, "maximized", lambda b: b is not None and
                               b[2] - b[0] > normal[2] - normal[0] + 100)
        probe.key("meta_l-z")
        wait_bounds(probe, process, "restored", lambda b: b == normal)
        probe.key("meta_l-m")
        wait_bounds(probe, process, "minimized", lambda b: b is None)
        probe.key("alt-tab")
        wait_bounds(probe, process, "unminimized", lambda b: b == normal)
        st_marker(probe, serial, process, "ICEWM-INTERACTIONS-END")
        metadata["completed"] = ["title drag", "Alt drag", "resize", "maximize/restore",
                                 "minimize/restore", "release", "terminal input"]
    except Exception as error:
        metadata["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        if probe:
            probe.close()
        if process.poll() is None:
            qmp_quit(qmp, process)
        metadata["source_unchanged"] = file_sha256(source) == source_hash
        metadata["private_base_unchanged"] = file_sha256(raw) == raw_hash
        (output / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
    assert metadata["source_unchanged"] and metadata["private_base_unchanged"]
    print("PASS IceWM interactions; source and snapshot base images unchanged")


if __name__ == "__main__":
    main()
