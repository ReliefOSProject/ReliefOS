#!/usr/bin/env python3
"""Sequential, private-disk dual-firmware acceptance with fresh runtime evidence."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import time
import uuid

from test_installer_accounts_qemu import (Probe, install_gui, install_tty,
                                        USER_PASSWORD, ROOT_PASSWORD, login_tty, installer_log)
from test_vt_qemu import wait_text

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    """Stream a source artifact hash without retaining its contents."""
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024**2), b""):
            result.update(block)
    return result.hexdigest()


def copy_disk(source, target):
    """Keep source snapshots immutable; each boot owns a separate raw working copy."""
    if target.exists():
        raise FileExistsError(target)
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", source, target], check=True)
    return target


def boot_evidence(text, firmware, release=None, *, cpus=None):
    """Require this firmware's kernel completion and actual PID 1 handoff."""
    expected = "UEFI" if firmware == "uefi" else "legacy"
    online = cpus is None or re.search(
        r"\[reliefnt\] boot complete:[^\r\n]*\bonline_cpus=" + str(cpus) + "/" + str(cpus) + r"(?:\s|$)", text)
    return bool(online) and (release is None or "version=" + release + " " in text) and all(marker in text for marker in (
        f"[boot] firmware={expected}", "[reliefnt] boot complete:",
        "[reliefnt] PID 1 path="))


def wait_for(serial, process, predicate, description, timeout):
    """Poll a fresh serial file; exit/timeout is failure, never success evidence."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial.read_text(errors="replace") if serial.exists() else ""
        if predicate(text):
            return text
        if process.poll() is not None:
            raise RuntimeError(f"QEMU exited before {description}: {process.returncode}")
        if "KERNEL PANIC" in text or "Kernel Panic" in text:
            raise AssertionError(f"Kernel panic before {description}")
        time.sleep(.2)
    raise AssertionError(f"Timed out waiting for {description}; see {serial}")


@contextmanager
def working_disk(disk, backend, *, size=3 * 1024**3, existing=False):
    """Use only a private ordinary file; guest cache flushes reach its backend."""
    if backend != "file":
        raise ValueError("Installation evidence requires an ordinary private disk file")
    if disk.exists() != existing:
        raise FileExistsError(f"Unexpected private disk state: {disk}")
    if not existing:
        with disk.open("xb") as stream:
            stream.truncate(size)
    yield disk


@contextmanager
def boot(output, firmware, *, disk=None, iso=None, timeout=180,
         nvme=False, extra_disk=None, optical_first=True, boot_extra=False,
         low_nvme_mmio=False):
    """Own one 4-GiB VM and its QMP socket, retaining fresh logs/screenshots."""
    output.mkdir(parents=True, exist_ok=False)
    serial = output / "serial.log"
    qmp = Path(os.environ.get("RELIEFOS_QMP_DIR", ROOT.parent)).resolve() / (".reliefos-ko-install-" + uuid.uuid4().hex[:12] + ".sock")
    if qmp.exists() or len(os.fsencode(qmp)) >= 104:
        raise ValueError("Choose an available short RELIEFOS_QMP_DIR on the project filesystem")
    command = [os.environ.get("QEMU", "qemu-system-x86_64"), "-machine", "q35",
               "-accel", os.environ.get("DUAL_BOOT_ACCEL", "kvm"),
               "-cpu", os.environ.get("DUAL_BOOT_CPU", "host"),
               "-m", "4096", "-smp", "2", "-display", "none",
               "-serial", f"file:{serial}", "-qmp", f"unix:{qmp},server=on,wait=off",
               "-device", "VGA,xres=1280,yres=800", "-device", "piix3-usb-uhci",
               "-device", "usb-tablet", "-netdev", "user,id=net0",
               "-device", "e1000,netdev=net0", "-no-reboot", "-no-shutdown"]
    if firmware == "uefi":
        image = Path(os.environ.get("QEMU_FIRMWARE", "/usr/share/edk2/x64/OVMF.4m.fd"))
        if not image.is_file():
            raise FileNotFoundError(f"UEFI firmware unavailable: {image}")
        command += ["-bios", str(image)]
    if nvme and low_nvme_mmio:
        # Existing kernel direct map is bounded to16GiB. This documented OVMF
        # platform setting places64-bit BARs in its32-bit aperture; high BAR
        # support is a separate kernel capability, never silently claimed here.
        command += ["-fw_cfg", "name=opt/ovmf/X-PciMmio64Mb,string=0"]
    if disk:
        command += ["-drive", f"file={disk},if=none,id=disk0,format=raw"]
        command += (["-device", "nvme,drive=disk0,serial=reliefos-dualboot"] if nvme else
                    ["-device", f"ide-hd,drive=disk0,bus=ide.0,bootindex={3 if boot_extra else 2}"])
    if extra_disk:
        command += ["-drive", f"file={extra_disk},if=none,id=disk1,format=raw",
                    "-device", f"ide-hd,drive=disk1,bus=ide.1,bootindex={2 if boot_extra else 3}"]
    if iso:
        command += ["-drive", f"file={iso},if=none,id=cd0,media=cdrom,format=raw",
                    "-device", f"ide-cd,drive=cd0,bus=ide.3,bootindex={1 if optical_first else 4}"]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "qemu.log").open("w") as errors:
        process = subprocess.Popen(command, cwd=ROOT, stdout=errors, stderr=errors)
        probe = None
        try:
            deadline = time.monotonic() + 15
            while not qmp.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"QEMU failed to start; see {output / 'qemu.log'}")
                time.sleep(.1)
            probe = Probe(qmp, output)
            probe.disk = disk
            yield probe, serial, process
        except Exception:
            if probe and process.poll() is None:
                try:
                    diagnostic = {"cpus": probe.command("human-monitor-command", {"command-line": "info cpus"}),
                                  "samples": []}
                    for _ in range(2):
                        diagnostic["samples"].append([
                            {"cpu": cpu,
                             "registers": probe.command("human-monitor-command", {"command-line": "info registers", "cpu-index": cpu}),
                             "stack": probe.command("human-monitor-command", {"command-line": "x/32gx $rsp", "cpu-index": cpu})}
                            for cpu in range(2)])
                        time.sleep(1)
                    (output / "failure-cpu-state.json").write_text(json.dumps(diagnostic, indent=2) + "\n")
                    probe.key("ctrl-alt-f2")
                    probe.frame("failure-tty2")
                except Exception as error:
                    (output / "failure-diagnostic-error.txt").write_text(str(error) + "\n")
            raise
        finally:
            try:
                if probe:
                    probe.frame("last-frame")
            except Exception as error:
                (output / "snapshot-error.txt").write_text(str(error) + "\n")
            finally:
                try:
                    if probe:
                        probe.close()
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                    qmp.unlink(missing_ok=True)


def source_disk(images, output):
    """Convert a source VMDK into a private raw disk; never open it for writes."""
    target = output / "shipped.raw"
    subprocess.run(["qemu-img", "convert", "-f", "vmdk", "-O", "raw",
                    str(images / "reliefos.vmdk"), str(target)], check=True)
    return target


def greeter_crop(frame):
    """Observe the centered XDM panel at its native size on large framebuffers."""
    width, height = min(frame.width, 1280), min(frame.height, 800)
    left, top = (frame.width - width) // 2, (frame.height - height) // 2
    return frame.crop((left, top, left + width, top + height)), left, top


def wait_xdm(probe, process, name, timeout, expected="login"):
    """Require two fresh, panel-scoped observations of the actual greeter state."""
    from test_xorg_vm_qemu import greeter_stage
    deadline, stable, observed = time.monotonic() + timeout, 0, "unknown"
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited before the XDM greeter"
        frame, _, _ = greeter_crop(probe.frame(name))
        frame.save(probe.output / f"{name}-crop.png")
        observed = greeter_stage(frame)
        stable = stable + 1 if observed == expected else 0
        if stable >= 2:
            return
        time.sleep(.5)
    raise AssertionError(f"Missing XDM {expected} state; observed {observed}")


def boot_matrix(args, output):
    """Reach the kernel and corresponding user environment on each same media."""
    results = []
    disk = source_disk(args.images, output) if "disk" in args.media.split(",") else None
    for firmware in args.firmwares.split(","):
        for medium in args.media.split(","):
            name = f"{firmware}-{medium}"
            record = {"case": name, "passed": False}
            try:
                with boot(output / name, firmware,
                          disk=copy_disk(disk, output / f"{name}.raw") if medium == "disk" else None,
                          iso=args.images / f"reliefos-{medium}.iso" if medium != "disk" else None) as (probe, serial, process):
                    wait_for(serial, process, lambda text: boot_evidence(text, firmware, args.expected_release, cpus=2),
                             f"{name} kernel/PID1", args.timeout)
                    if medium == "installer":
                        wait_for(serial, process, lambda text: "path=/usr/lib/reliefos/apps/installer/installer.elf" in text,
                                 "installer executable", args.timeout)
                        deadline = time.monotonic() + args.timeout
                        while time.monotonic() < deadline:
                            if probe.visible_page(probe.frame("installer")) == 0:
                                break
                            time.sleep(.5)
                        else:
                            raise AssertionError("Installer language page did not appear")
                    else:
                        wait_for(serial, process, lambda text: "VT graphics vt=1 enabled=1" in text,
                                 "graphical login", args.timeout)
                        wait_xdm(probe, process, "login", args.timeout)
                    record["passed"] = True
            except Exception as error:
                record["error"] = str(error)
            results.append(record)
            print(json.dumps(record), flush=True)
            (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    return results


def inspect_install(disk, output, bios, uefi):
    """Check actual GPT and loader records after the installing VM has exited."""
    table = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
    parts = table["partitions"]
    assert len(parts) == 3 and parts[0]["start"] == 2048 and parts[0]["size"] == 2048
    assert parts[0]["type"].lower() == "21686148-6449-6e6f-744e-656564454649"
    esp = next(p for p in parts if p["type"].lower() == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
    root = next(p for p in parts if p["type"].lower() == "0fc63daf-8483-4772-8e79-3d69d8477de4")
    source = f"{disk}@@{esp['start'] * 512}"
    record = subprocess.check_output(["mtype", "-i", source, "::/reliefos/config/bootloaders.conf"], text=True)
    assert record == f"version=1\nbios={int(bios)}\nuefi={int(uefi)}\n", record
    binding = subprocess.check_output(["mtype", "-i", source, "::/reliefos/config/root.cfg"], text=True)
    assert binding.strip() == f"set reliefos_root=PARTUUID={root['uuid'].lower()}"
    efi = subprocess.run(["mdir", "-i", source, "::/EFI/BOOT/BOOTX64.EFI"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    assert efi == uefi
    with disk.open("rb") as stream:
        assert any(stream.read(440)) == bios
        stream.seek(2048 * 512)
        assert any(stream.read(2048 * 512)) == bios
    report = {"disk_guid": table["id"], "root_partuuid": root["uuid"],
              "esp_partuuid": esp["uuid"], "bios": bios, "uefi": uefi}
    (output / f"{disk.stem}-layout.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def desktop_session(probe, serial, process, timeout):
    """Authenticate at real XDM, then run Fileman in the Chinese IceWM session."""
    from test_xorg_vm_qemu import _session_st_visible, _bright_panel_bbox
    wait_xdm(probe, process, "xdm-login", timeout)
    cropped, left, top = greeter_crop(probe.frame("xdm-panel"))
    box = _bright_panel_bbox(cropped)
    if box:
        probe.click(left + (box[0] + box[2]) // 2, top + (box[1] + box[3]) // 2)
    probe.text("alice")
    probe.key("ret")
    wait_xdm(probe, process, "xdm-password", timeout, expected="password")
    probe.text(USER_PASSWORD)
    probe.key("ret")
    wait_for(serial, process, lambda text: "path=/usr/bin/icewm-session" in text,
             "authenticated IceWM", timeout)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _session_st_visible(probe.frame("icewm-desktop")):
            break
        time.sleep(.5)
    else:
        raise AssertionError("Authenticated desktop terminal did not map")
    probe.text("fileman &")
    probe.key("ret")
    wait_for(serial, process, lambda text: "path=/usr/lib/reliefos/apps/fileman/fileman.elf" in text,
             "Fileman on installed desktop", timeout)
    time.sleep(2)
    probe.frame("icewm-fileman-chinese")


def prepare_install_disk(disk, name, output):
    """Exercise default repartitioning over an old two-partition FAT ESP signature."""
    if name != "bios-only":
        return disk
    fixture = output / "old-esp.fat"
    with fixture.open("xb") as stream:
        stream.truncate(128 * 1024**2)
    subprocess.run(["mkfs.fat", "-F", "32", fixture], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["sfdisk", disk], input="label: gpt\nunit: sectors\n\n"
                   "start=2048,size=262144,type=U\nstart=264192,type=L\n",
                   text=True, check=True, stdout=subprocess.DEVNULL)
    table = json.loads(subprocess.check_output(["sfdisk", "--json", disk]))["partitiontable"]
    assert len(table["partitions"]) == 2 and table["partitions"][0]["start"] == 2048
    (output / "old-layout-before.json").write_text(json.dumps(table, indent=2) + "\n")
    with fixture.open("rb") as source, disk.open("r+b") as target:
        target.seek(1024**2)
        target.write(source.read(1024**2))
        target.flush()
        os.fsync(target.fileno())
    return disk


def install_matrix(args, output):
    """Install through GUI/TTY, remove ISO, and boot each selected mode's disk."""
    choices = {"bios-both": ("bios", True, True, "tty"),
               "uefi-both": ("uefi", True, True, "gui"),
               "bios-only": ("bios", True, False, "tty"),
               "uefi-only": ("uefi", False, True, "tty"),
               "none": ("bios", False, False, "tty")}
    results, identities = [], set()
    for name in args.install_cases.split(","):
        firmware, bios, uefi, frontend = choices[name]
        record = {"case": name, "passed": False}
        disk = output / f"{name}.raw"
        try:
            with (working_disk(disk, args.disk_backend) as transient,
                  boot(output / f"{name}-install", firmware,
                       disk=prepare_install_disk(transient, name, output),
                       iso=args.images / "reliefos-installer.iso") as session):
                try:
                    if frontend == "gui":
                        install_gui(*session, chinese=True, install_timeout=args.install_timeout)
                    else:
                        install_tty(*session, skip_xorg=True,
                                    install_bios=bios, install_uefi=uefi,
                                    install_timeout=args.install_timeout)
                except Exception:
                    try:
                        installer_log(session[0], session[1])
                    except Exception as error:
                        (session[0].output / "log-extraction-error.txt").write_text(str(error) + "\n")
                    raise
            layout = inspect_install(disk, output, bios, uefi)
            assert layout["root_partuuid"] not in identities, "Fresh installs reused a root UUID"
            identities.add(layout["root_partuuid"])
            record["layout"] = layout
            snapshot_hash = digest(disk)
            record["installed_snapshot_sha256"] = snapshot_hash
            if name == "bios-only":
                record["repartitioned_old_esp_signature"] = True
            for target in ("bios", "uefi"):
                expected = bios if target == "bios" else uefi
                boot_disk = copy_disk(disk, output / f"{name}-{target}.raw")
                with boot(output / f"{name}-{target}", target, disk=boot_disk) as (probe, serial, process):
                    if expected:
                        wait_for(serial, process, lambda text: boot_evidence(text, target, args.expected_release, cpus=2),
                                 f"installed {name}/{target}", args.timeout)
                        if frontend == "gui":
                            wait_for(serial, process, lambda text: "VT graphics vt=1 enabled=1" in text,
                                     "installed graphical VT", args.timeout)
                            wait_xdm(probe, process, "xdm", args.timeout)
                            if target == "bios":
                                desktop_session(probe, serial, process, args.timeout)
                        else:
                            wait_text(probe, process, "console-login", "login:", timeout=args.timeout)
                            if target == firmware:
                                login_tty(probe, serial, process, True, skip_xorg=True)
                    else:
                        try:
                            wait_for(serial, process, lambda text: "[reliefnt] boot complete:" in text,
                                     "expected absent loader", args.negative_timeout)
                        except AssertionError as error:
                            if not str(error).startswith("Timed out"):
                                raise
                        else:
                            raise AssertionError(f"Unselected {target} loader unexpectedly booted")
                        probe.frame("expected-no-loader")
            assert digest(disk) == snapshot_hash, "Booting changed the immutable installation snapshot"
            record["passed"] = True
        except Exception as error:
            record["error"] = str(error)
        results.append(record)
        print(json.dumps(record), flush=True)
        (output / "install-results.json").write_text(json.dumps(results, indent=2) + "\n")
        if not record["passed"]:
            break
    return results


def self_test():
    """Prove banners, missing completion and wrong firmware cannot pass."""
    complete = "[boot] firmware=legacy\n[reliefnt] boot complete:\n[reliefnt] PID 1 path=/init\n"
    assert boot_evidence(complete, "bios")
    assert not boot_evidence(complete, "uefi")
    assert not boot_evidence(complete.replace("[reliefnt] boot complete:", ""), "bios")
    assert not boot_evidence("GNU GRUB 2.12\n", "bios")
    assert not boot_evidence(complete, "bios", "wrong-release")
    assert boot_evidence(complete + "version=matching-release root=/\n", "bios", "matching-release")
    assert not boot_evidence(complete, "bios", cpus=2)
    assert not boot_evidence(complete.replace("boot complete:", "boot complete: online_cpus=1/2"), "bios", cpus=2)
    assert boot_evidence(complete.replace("boot complete:", "boot complete: online_cpus=2/2"), "bios", cpus=2)
    from test_installer_accounts_qemu import wait_install
    from unittest.mock import patch
    import tempfile
    class FinishProbe:
        """Keep UI and success-channel assertions independent of real guest I/O."""
        def frame(self, name):
            return name
        def visible_page(self, frame):
            return 9
    class Process:
        def poll(self):
            return None
    with tempfile.TemporaryDirectory() as directory:
        serial = Path(directory) / "serial.log"
        for text, success in (
                ("GNU GRUB 2.12", False),
                ("Reboot now?", False),
                ("[installer.elf] copy failed ret=-5", False),
                ("[installer.elf] installation completed successfully\nReboot now?", True)):
            serial.write_text(text)
            for tty in (True, False):
                try:
                    with patch("test_installer_accounts_qemu.time.monotonic", side_effect=(0, 0, 2401)), \
                         patch("test_installer_accounts_qemu.time.sleep"), \
                         patch("test_installer_accounts_qemu.installer_finish_log", return_value=text):
                        wait_install(FinishProbe(), serial, Process(), tty=tty)
                except AssertionError:
                    assert not success, (text, tty)
                else:
                    assert success, (text, tty)
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        for backend in ("file",):
            disk = Path(directory) / f"{backend}.raw"
            with working_disk(disk, backend, size=4 * 1024**2) as transient:
                with transient.open("r+b") as stream:
                    stream.seek(2 * 1024**2)
                    stream.write(b"real-disk-bytes")
            assert disk.stat().st_size == 4 * 1024**2
            with disk.open("rb") as stream:
                stream.seek(2 * 1024**2)
                assert stream.read(15) == b"real-disk-bytes"
            with working_disk(disk, backend, existing=True) as transient:
                with transient.open("r+b") as stream:
                    stream.seek(2 * 1024**2)
                    assert stream.read(15) == b"real-disk-bytes"
                    stream.seek(2 * 1024**2)
                    stream.write(b"updated-content")
            with disk.open("rb") as stream:
                stream.seek(2 * 1024**2)
                assert stream.read(15) == b"updated-content"
        serial = Path(directory) / "serial.log"
        serial.write_text("GNU GRUB 2.12\n")
        class Running:
            def poll(self):
                return None
        try:
            wait_for(serial, Running(), lambda text: boot_evidence(text, "bios"), "fixture", .01)
        except AssertionError:
            pass
        else:
            raise AssertionError("Missing completion fixture unexpectedly passed")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory)
        for backend in ("file",):
            disk = output / f"old-layout-{backend}.raw"
            case = output / backend
            case.mkdir()
            with working_disk(disk, backend, size=384 * 1024**2) as transient:
                assert prepare_install_disk(transient, "bios-only", case) == transient
                table = json.loads(subprocess.check_output(["sfdisk", "--json", transient]))["partitiontable"]
                assert len(table["partitions"]) == 2 and table["partitions"][0]["start"] == 2048
                with transient.open("rb") as stream:
                    stream.seek(1024**2 + 82)
                    assert stream.read(8) == b"FAT32   "
    print("PASS runner refuses incomplete/failed results, saves disk bytes, and seeds real old FAT layouts", flush=True)


def main():
    """Serialize this task's VM ownership and save per-run artifact identities."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, default=ROOT / "out/dualboot-build/images")
    parser.add_argument("--kernel-manifest", type=Path, help="require this install.bundle.json kernel release on every positive boot")
    parser.add_argument("--out", type=Path, default=ROOT / "out/dbq")
    parser.add_argument("--matrix", choices=("boot", "install", "regression", "all"), default="all")
    parser.add_argument("--console-disk", type=Path, help="Existing accepted console install for regression-only runs")
    parser.add_argument("--desktop-disk", type=Path, help="Existing accepted desktop install for regression-only runs")
    parser.add_argument("--legacy-disk", type=Path, default=ROOT / "out/kbuild-verified/images/reliefos.raw")
    parser.add_argument("--firmwares", default="bios,uefi")
    parser.add_argument("--media", default="disk,live,installer")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--negative-timeout", type=int, default=20)
    parser.add_argument("--disk-backend", choices=("file",), default="file",
                        help="Private installation backing; all installed boots use saved raw files")
    parser.add_argument("--install-timeout", type=int, default=2400,
                        help="Finite copy/flush budget in seconds; boot deadlines remain separate")
    parser.add_argument("--install-cases", default="bios-both,uefi-both,bios-only,uefi-only,none")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.install_timeout <= 0:
        parser.error("--install-timeout must be positive")
    args.expected_release = None
    if args.kernel_manifest:
        args.expected_release = json.loads(args.kernel_manifest.read_text())["release"]
        if not re.fullmatch(r"[A-Za-z0-9._+-]{1,31}", args.expected_release):
            parser.error("invalid expected kernel release")
    if args.timeout < 1 or args.negative_timeout < 1:
        parser.error("timeouts must be positive")
    if args.self_test:
        self_test()
        return
    if not set(args.firmwares.split(",")) <= {"bios", "uefi"}:
        parser.error("firmwares must contain bios and/or uefi")
    if not set(args.media.split(",")) <= {"disk", "live", "installer"}:
        parser.error("media must contain disk, live and/or installer")
    if not set(args.install_cases.split(",")) <= {"bios-both", "uefi-both", "bios-only", "uefi-only", "none"}:
        parser.error("Unknown install case")
    args.images = args.images.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "runner.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        output = args.out / time.strftime("%m%d-%H%M%S")
        output.mkdir()
        hashes = {path.name: digest(path) for path in [
            args.images / "reliefos.vmdk", args.images / "reliefos-live.iso",
            args.images / "reliefos-installer.iso"] if path.is_file()}
        (output / "sources.json").write_text(json.dumps(hashes, indent=2) + "\n")
        (output / "settings.json").write_text(json.dumps({"installation_disk_backend": args.disk_backend,
                                                          "guest_memory_mib": 4096,
                                                          "expected_release": args.expected_release}, indent=2) + "\n")
        results = boot_matrix(args, output) if args.matrix in ("boot", "all") else []
        if args.matrix in ("install", "all") and all(result["passed"] for result in results):
            results += install_matrix(args, output)
        if args.matrix in ("regression", "all") and all(result["passed"] for result in results):
            from test_dual_boot_regressions import regression_matrix
            results += regression_matrix(args, output)
        after = {name: digest(args.images / name) for name in hashes}
        (output / "sources-after.json").write_text(json.dumps(after, indent=2) + "\n")
        assert after == hashes, "A source installation/boot medium changed during acceptance"
        if not all(result["passed"] for result in results):
            raise SystemExit(1)


if __name__ == "__main__":
    main()
