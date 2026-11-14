#!/usr/bin/env python3
"""Exercise the actual update UI/cache on a private copy of a built disk."""
import argparse
import json
import re
from pathlib import Path
import subprocess
import time

from test_dual_boot_qemu import boot, copy_disk, digest, wait_for
from test_installer_accounts_qemu import next_page, wait_installer_sidebar
from test_vt_qemu import ocr

ROOT = Path(__file__).resolve().parents[1]


def grow_private_root(disk, output):
    """Give the private test root update headroom without altering any identity."""
    table = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
    roots = [(i, part) for i, part in enumerate(table["partitions"], 1)
             if part["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4"]
    assert len(roots) == 1 and roots[0][0] == len(table["partitions"])
    index, root = roots[0]
    sectors = 3 * 1024**3 // 512
    if root["size"] >= sectors:
        return
    image = output / "expanded-root.ext4"
    subprocess.run(["dd", f"if={disk}", f"of={image}", "bs=1M", "iflag=skip_bytes,count_bytes",
                    f"skip={root['start'] * 512}", f"count={root['size'] * 512}", "conv=sparse", "status=none"], check=True)
    with image.open("r+b") as stream:
        stream.truncate(sectors * 512)
    subprocess.run(["resize2fs", str(image)], check=True)
    with disk.open("r+b") as stream:
        stream.truncate((root["start"] + sectors + 2048) * 512)
    subprocess.run(["sfdisk", "--no-reread", "-N", str(index), str(disk)],
                   input=f"size={sectors}\n", text=True, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["dd", f"if={image}", f"of={disk}", "bs=1M", "oflag=seek_bytes",
                    f"seek={root['start'] * 512}", "conv=notrunc", "status=none"], check=True)
    image.unlink()
    updated = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
    assert updated["partitions"][index - 1]["uuid"] == root["uuid"]


def guest_log(probe, serial, process):
    """Read the live update log; cached filesystem snapshots cannot prove success."""
    before = serial.stat().st_size
    marker = "__UPDATE_CACHE_LOG_END__"
    probe.key("ctrl-alt-f2")
    time.sleep(1)
    probe.text("cat /var/log/installer-update.log > /dev/ttyS0; echo " + marker + " > /dev/ttyS0")
    probe.key("ret")
    wait_for(serial, process, lambda text: marker in text, "live installer update log", 90)
    text = serial.read_bytes()[before:].decode(errors="replace")
    (probe.output / "update-live.log").write_text(text)
    probe.key("ctrl-alt-f1")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, default=ROOT / "out/x86_64/release/images")
    parser.add_argument("--out", type=Path, default=ROOT / "out/installer-update-cache-qa")
    parser.add_argument("--firmware", choices=("bios", "uefi"), default="bios")
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    images = args.images.resolve()
    output = args.out.resolve() / time.strftime("%m%d-%H%M%S")
    output.mkdir(parents=True, exist_ok=False)
    source = images / "reliefos.raw"
    iso = images / "reliefos-installer.iso"
    sources = {str(path): digest(path) for path in (source, iso)}
    (output / "sources.json").write_text(json.dumps(sources, indent=2) + "\n")
    disk = copy_disk(source, output / "private-update.raw")
    grow_private_root(disk, output)
    observed_cache = False
    with boot(output / "guest", args.firmware, disk=disk, iso=iso) as (probe, serial, process):
        wait_for(serial, process, lambda text: "path=/usr/lib/reliefos/apps/installer/installer.elf" in text,
                 "native installer", 180)
        wait_installer_sidebar(probe, process, "language")
        if probe.sidebar_bottom - probe.sidebar_top < 680:
            probe.key("alt-f10")
            time.sleep(1)
            probe.clear_installer_geometry()
            wait_installer_sidebar(probe, process, "language-maximized")
        for page in (1, 2, 3):
            next_page(probe, page)
        probe.click(*probe.mode_option_point(update=True))
        next_page(probe, 4)
        probe.click(*probe.disk_selection_point())
        probe.click(*probe.footer_point())
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            assert process.poll() is None
            frame = probe.frame("checking")
            page = probe.visible_page(frame)
            if page == 5:  # Update has a compact sidebar: confirmation follows disk.
                break
            if page == 7:
                raise AssertionError("Update checks failed: " + guest_log(probe, serial, process)[-6000:])
            time.sleep(2)
        else:
            raise AssertionError("Update checks timed out")
        probe.frame("confirmed-target")
        probe.click(*probe.footer_point())
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            assert process.poll() is None
            frame = probe.frame("update-progress")
            page = probe.visible_page(frame)
            if page == 7:
                break
            if page == 6:
                details = frame.crop((probe.content_left, probe.sidebar_top + 202,
                                      min(frame.width, probe.content_left + 790), probe.sidebar_top + 286))
                text = ocr(details)
                with (output / "activity-observations.txt").open("a") as stream:
                    stream.write(text + "\n---\n")
                details.save(output / "activity-last.png")
                lowered = text.lower()
                if re.search(r"[0-9]+\s*/\s*[0-9]+", text) and ("mib" in lowered or "processed" in lowered) and \
                   ("apk" in lowered or "packages.adb" in lowered):
                    observed_cache = True
                    details.save(output / "cache-details.png")
                    frame.save(output / "cache-progress.png")
            time.sleep(2)
        else:
            raise AssertionError("Actual native update timed out")
        live = guest_log(probe, serial, process)
        assert "System update completed successfully" in live, live[-6000:]
        events = []
        for line in live.splitlines():
            fields = line.split("\t")
            if fields[:2] == ["activity", "cache"] and len(fields) == 10:
                events.append(fields)
        assert events and observed_cache, "Actual cache counters were not observed in the log and UI"
        done, total, processed, size, reused, status = map(int, events[-1][4:])
        assert done == total and processed == size and size > 0 and reused > 0 and status == 0
        probe.frame("update-complete")
        result = {"passed": True, "firmware": args.firmware, "files": total,
                  "processed_bytes": size, "reused_bytes": reused, "cache_ui_observed": observed_cache}
    assert sources == {str(path): digest(path) for path in (source, iso)}, "Source images changed"
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
