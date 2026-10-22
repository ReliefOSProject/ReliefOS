#!/usr/bin/env python3
"""Real private-disk regressions shared by the sequential dual-boot runner."""
import json
from pathlib import Path
import re
import subprocess
import time

from test_dual_boot_qemu import ROOT, boot, boot_evidence, digest, wait_for, source_disk, wait_xdm, working_disk, copy_disk
from test_installer_accounts_qemu import (login_tty, next_page, installer_log,
                                         installer_finish_log, wait_installer_sidebar)
from test_vt_qemu import ocr, wait_text


def table_for(disk):
    """Read GPT through an independent standard host tool."""
    return json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]


def root_identity(disk):
    """Resolve the sole test root partition without assuming its GPT slot."""
    return next(part["uuid"].lower() for part in table_for(disk)["partitions"]
                if part["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4")


def platform_evidence(text):
    """Check actual SMP, usable RAM, ACPI, framebuffer and physical-root handoff."""
    assert "online_cpus=2/2" in text
    free = re.search(r"free_kib=(\d+)", text)
    assert free and int(free[1]) > 0
    assert re.search(r"acpi_rsdp=0x[1-9a-f][0-9a-f]*", text)
    assert "ACPI S5 shutdown ready" in text
    assert re.search(r"framebuffer \d+x\d+ pitch=\d+ bpp=(24|32)", text)
    assert "root_ready=1 filesystem=ext4" in text


def rollback_menu(probe, process, expected):
    """Observe the actual menu's final entries, stopping its auto-boot countdown."""
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited before GRUB menu"
        probe.key("up")
        frame = probe.frame("grub-menu")
        visible = ocr(frame).lower().replace("0", "o")
        if "select an operating system" in visible:
            break
    else:
        raise AssertionError("GRUB menu not observed before rollback test")
    probe.key("end")
    visible = ocr(probe.frame("rollback-menu")).lower().replace("0", "o")
    assert "reboot" in visible, visible
    assert ("legacy rollback" in visible) == expected, visible
    if expected:
        probe.key("up")  # The guarded rollback entry immediately precedes Reboot.
    else:
        probe.key("home")
    probe.key("ret")


def two_disk_boots(args, output, console, desktop):
    """Boot the second physical SATA disk despite matching paths on the first."""
    for firmware in ("bios", "uefi"):
        for name, first, second, graphical in (
                ("console", desktop, console, False),
                ("desktop", console, desktop, True)):
            case = output / f"order-{firmware}-{name}"
            first_copy = copy_disk(first, output / f"order-{firmware}-{name}-first.raw")
            second_copy = copy_disk(second, output / f"order-{firmware}-{name}-second.raw")
            with boot(case, firmware, disk=first_copy, extra_disk=second_copy,
                      boot_extra=True) as (probe, serial, process):
                if not graphical:
                    rollback_menu(probe, process, expected=True)
                text = wait_for(serial, process, lambda value: boot_evidence(value, firmware),
                                "second-disk kernel/PID1", args.timeout)
                assert f"cmdline=root=PARTUUID={root_identity(second)} " in text
                platform_evidence(text)
                if graphical:
                    wait_for(serial, process, lambda value: "VT graphics vt=1 enabled=1" in value,
                             "selected desktop root", args.timeout)
                    wait_xdm(probe, process, "selected-root", args.timeout)
                else:
                    wait_text(probe, process, "selected-root", "login:", timeout=args.timeout)
                    assert "path=/usr/bin/xdm" not in serial.read_text(errors="replace")


def duplicate_identity(args, output, console):
    """A cloned PARTUUID is ambiguous and must fail before PID1, never fall back."""
    clone = copy_disk(console, output / "duplicate.raw")
    with boot(output / "duplicate-identity", "bios", disk=console,
              extra_disk=clone) as (probe, serial, process):
        text = wait_for(serial, process, lambda value: "explicit root selection failed" in value,
                        "duplicate root refusal", args.timeout)
        assert "status=-76; refusing automatic fallback" in text, text[-5000:]
        assert "[reliefnt] PID 1 path=" not in text
        probe.frame("duplicate-refused")


def console_and_reboot(args, output, console):
    """Exercise root/user DAC and real shell I/O, then request an actual reset."""
    with boot(output / "console-io-reboot", "bios", disk=console) as (probe, serial, process):
        text = wait_for(serial, process, lambda value: boot_evidence(value, "bios"),
                        "console root", args.timeout)
        platform_evidence(text)
        login_tty(probe, serial, process, True, skip_xorg=True)
        root_log = probe.output / "session-evidence.txt"
        root_log.rename(probe.output / "root-session-evidence.txt")
        probe.text("exit")
        probe.key("ret")
        login_tty(probe, serial, process, False, skip_xorg=True)
        (probe.output / "session-evidence.txt").rename(probe.output / "user-session-evidence.txt")
        probe.text("exit")
        probe.key("ret")
        wait_text(probe, process, "root-reset-login", "login:")
        probe.text("root")
        probe.key("ret")
        wait_text(probe, process, "root-reset-password", "Password:")
        probe.text("r")
        probe.key("ret")
        time.sleep(1)
        probe.text("sync; reboot")
        probe.key("ret")
        wait_for(serial, process, lambda value: "[reliefnt] reboot requested" in value,
                 "guest reset request", 30)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            state = probe.command("query-status")
            if state.get("status") == "shutdown":
                break
            time.sleep(.2)
        else:
            raise AssertionError("Guest requested reset but QEMU never observed it")


def legacy_fixture(source, target, output):
    """Expand an actual old two-partition UEFI image without adding a BIOS slot."""
    original = table_for(source)
    assert len(original["partitions"]) == 2, "Legacy fixture must predate dual boot"
    esp, root = original["partitions"]
    assert esp["type"].lower() == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
    assert root["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4"
    with target.open("xb") as stream:
        stream.truncate(3 * 1024**3)
    script = (f"label: gpt\nlabel-id: {original['id']}\n"
              f"start={esp['start']},size={esp['size']},type={esp['type']},uuid={esp['uuid']}\n"
              f"start={root['start']},type={root['type']},uuid={root['uuid']}\n")
    subprocess.run(["sfdisk", target], input=script, text=True, check=True,
                   stdout=subprocess.DEVNULL)
    new_root = table_for(target)["partitions"][1]
    root_image = output / "legacy-root.ext4"
    subprocess.run(["dd", f"if={source}", f"of={root_image}", "bs=4M",
                    "iflag=skip_bytes,count_bytes", f"skip={root['start'] * 512}",
                    f"count={root['size'] * 512}", "conv=sparse", "status=none"], check=True)
    with root_image.open("r+b") as stream:
        stream.truncate((new_root["size"] * 512 // 4096) * 4096)
    checked = subprocess.run(["e2fsck", "-fy", root_image], stdout=subprocess.DEVNULL)
    assert checked.returncode in (0, 1), "Legacy filesystem fixture is not repairable"
    subprocess.run(["resize2fs", root_image], check=True, stdout=subprocess.DEVNULL)
    sentinel = output / "legacy-user-file"
    sentinel.write_text("preserve-me-across-dual-boot-upgrade\n")
    subprocess.run(["debugfs", "-w", "-R", f"write {sentinel} /root/dual-boot-preserve", root_image],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["dd", f"if={root_image}", f"of={target}", "bs=4M", "oflag=seek_bytes",
                    f"seek={root['start'] * 512}", "conv=notrunc,sparse", "status=none"], check=True)
    subprocess.run(["dd", f"if={source}", f"of={target}", "bs=4M", "iflag=skip_bytes,count_bytes",
                    "oflag=seek_bytes", f"skip={esp['start'] * 512}", f"seek={esp['start'] * 512}",
                    f"count={esp['size'] * 512}", "conv=notrunc,sparse", "status=none"], check=True)
    root_image.unlink()
    return table_for(target)


def legacy_update(args, output):
    """Reject BIOS-without-space, then preserve an actual old UEFI installation."""
    disk = output / "legacy.raw"
    before = legacy_fixture(args.legacy_disk.resolve(), disk, output)
    esp = before["partitions"][0]
    fat = f"{disk}@@{esp['start'] * 512}"
    old_kernel = subprocess.check_output(["mtype", "-i", fat, "::/reliefos/kernel.sys"])
    before_hash = digest(disk)
    # This actual pre-migration UEFI-only disk must fail the BIOS boot gate.
    with boot(output / "legacy-bios-negative", "bios", disk=disk) as (probe, serial, process):
        try:
            wait_for(serial, process, lambda value: boot_evidence(value, "bios"),
                     "old UEFI-only disk BIOS handoff", args.negative_timeout)
        except AssertionError as error:
            if not str(error).startswith("Timed out"):
                raise
        else:
            raise AssertionError("Old UEFI-only media unexpectedly passed the BIOS gate")
        probe.frame("expected-old-bios-failure")
    assert digest(disk) == before_hash
    with (working_disk(disk, args.disk_backend, existing=True) as transient,
          boot(output / "legacy-check", "bios", disk=transient,
               iso=args.images / "reliefos-installer.iso") as (probe, serial, process)):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            probe.key("up")
            if "install reliefos" in ocr(probe.frame("console-menu")).lower().replace("0", "o"):
                break
        else:
            raise AssertionError("Legacy preflight could not select the console entry")
        probe.key("down")
        probe.key("ret")
        wait_for(serial, process, lambda value: "path=/bin/bash" in value,
                 "installer rescue shell", args.timeout)
        command = ("( mkdir -p /mnt/boot; mount -t vfat /dev/sda1 /mnt/boot; "
                   "reliefos-boot-install --source /install/esp --boot-directory /mnt/boot "
                   "--disk /dev/sda --bios=yes --uefi=yes --check; "
                   "echo DUAL_CHECK_RC=$?; umount /mnt/boot; sync ) 2>&1 | tee /dev/ttyS0")
        probe.text(command)
        probe.key("ret")
        text = wait_for(serial, process, lambda value: "DUAL_CHECK_RC=1" in value,
                        "missing BIOS-space refusal", 60)
        assert "reliefos-boot-install:" in text and "(-28)" in text, text[-4000:]
        probe.frame("no-bios-space-refused")
    assert digest(disk) == before_hash, "Rejected BIOS request changed the old installation"
    with (working_disk(disk, args.disk_backend, existing=True) as transient,
          boot(output / "legacy-update", "uefi", disk=transient,
               iso=args.images / "reliefos-installer.iso") as (probe, serial, process)):
        wait_for(serial, process, lambda value: "path=/usr/lib/reliefos/apps/installer/installer.elf" in value,
                 "actual update GUI", args.timeout)
        wait_installer_sidebar(probe, process, "language", args.timeout)
        for _ in range(3):
            next_page(probe)
        probe.click(*probe.mode_option_point(update=True))
        next_page(probe)
        next_page(probe)
        probe.click(*probe.confirm_field_point())
        probe.text("UPDATE")
        probe.frame("legacy-confirm-uefi-only")
        next_page(probe)
        deadline = time.monotonic() + 2400
        while time.monotonic() < deadline:
            assert process.poll() is None, "QEMU exited during the GUI update"
            log = serial.read_text(errors="replace")
            if any("[installer.elf]" in line and " ret=-" in line for line in log.splitlines()):
                raise AssertionError("Actual legacy update failed; see serial.log")
            # Update omits the Partitions/Accounts pages: Finish is row7,
            # whereas the fresh-install Finish page is row9.
            if probe.visible_page(probe.frame("update-progress")) == 7:
                current = installer_finish_log(probe, serial, process)
                assert "[installer.elf] update completed successfully" in current, current[-10000:]
                assert "APK update wait_status=0" in current, current[-10000:]
                probe.frame("updated")
                break
            time.sleep(5)
        else:
            raise AssertionError("Legacy GUI update timed out")
    assert table_for(disk) == before, "UEFI update changed old GPT geometry/identities"
    rollback = subprocess.check_output(["mtype", "-i", fat, "::/leonos/kernel.sys"])
    assert rollback == old_kernel, "Old kernel was not preserved for rollback"
    modes = subprocess.check_output(["mtype", "-i", fat, "::/reliefos/config/bootloaders.conf"], text=True)
    assert modes == "version=1\nbios=0\nuefi=1\n", modes
    root = before["partitions"][1]
    root_image = output / "updated-root.ext4"
    subprocess.run(["dd", f"if={disk}", f"of={root_image}", "bs=4M", "iflag=skip_bytes,count_bytes",
                    f"skip={root['start'] * 512}", f"count={root['size'] * 512}",
                    "conv=sparse", "status=none"], check=True)
    preserved = subprocess.check_output(["debugfs", "-R", "cat /root/dual-boot-preserve", root_image],
                                        stderr=subprocess.DEVNULL, text=True)
    assert preserved == "preserve-me-across-dual-boot-upgrade\n"
    root_image.unlink()
    current_kernel = subprocess.check_output(["mtype", "-i", fat, "::/reliefos/kernel.sys"])
    assert current_kernel != old_kernel, "Old-kernel menu refusal needs genuinely different bytes"
    with boot(output / "legacy-updated-uefi", "uefi", disk=disk) as (probe, serial, process):
        rollback_menu(probe, process, expected=False)
        text = wait_for(serial, process, lambda value: boot_evidence(value, "uefi"),
                        "updated legacy disk without ISO", args.timeout)
        assert f"cmdline=root=PARTUUID={root['uuid'].lower()} " in text
        platform_evidence(text)
        wait_xdm(probe, process, "updated-xdm", args.timeout)


def regression_matrix(args, output):
    """Retain one JSON result for each actual runtime regression."""
    results = []
    console = args.console_disk or output / "bios-both.raw"
    desktop = args.desktop_disk or output / "uefi-both.raw"
    for name, action in (
            ("two-disk-order", lambda: two_disk_boots(args, output,
                copy_disk(console, output / "order-console.raw"),
                copy_disk(desktop, output / "order-desktop.raw"))),
            ("duplicate-partuuid", lambda: duplicate_identity(args, output,
                copy_disk(console, output / "identity-console.raw"))),
            ("tty-permissions-io-reboot", lambda: console_and_reboot(args, output,
                copy_disk(console, output / "io-console.raw"))),
            ("uefi-nvme-low-mmio", lambda: nvme_boot(args, output)),
            ("legacy-uefi-update", lambda: legacy_update(args, output))):
        record = {"case": name, "passed": False}
        try:
            action()
            record["passed"] = True
        except Exception as error:
            record["error"] = str(error)
        results.append(record)
        print(json.dumps(record), flush=True)
        (output / "regression-results.json").write_text(json.dumps(results, indent=2) + "\n")
    return results


def nvme_boot(args, output):
    """Validate the shipped kernel/root through OVMF's standard NVMe path."""
    shipped = output / "shipped.raw"
    disk = (copy_disk(shipped, output / "nvme.raw") if shipped.exists()
            else source_disk(args.images, output))
    with boot(output / "uefi-nvme-low-mmio", "uefi", disk=disk, nvme=True,
              low_nvme_mmio=True) as (probe, serial, process):
        text = wait_for(serial, process, lambda value: boot_evidence(value, "uefi"),
                        "UEFI NVMe kernel/PID1", args.timeout)
        platform_evidence(text)
        assert "transport=nvme" in text
        wait_for(serial, process, lambda value: "VT graphics vt=1 enabled=1" in value,
                 "NVMe graphical VT", args.timeout)
        wait_xdm(probe, process, "nvme-xdm", args.timeout)
