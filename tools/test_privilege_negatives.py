#!/usr/bin/env python3
"""Privilege negative guest regression for the ntclks separation (phase 4).

Boots a disposable copy of a ReliefOS image in QEMU and pins today's privilege
boundaries from docs/superpowers/ntclks-separation/03-permission-matrix.md
BEFORE any decoupling work (plan rule: negatives first, never remove a check
before the tests exist):

  M1/M14  service impersonation negatives (non-root exec at the service paths,
          user-writable copies at service-looking paths), the M3 fork-child
          flag stripping, and a positive control that the M1 grant itself
          still happens (so the negatives cannot pass vacuously)
  M2      exec drops TASK_FLAG_SERVICE / TASK_FLAG_WINDOW_SERVER
  M4      setuid-root exec observed through AT_SECURE, the euid transition
          and PR_GET_DUMPABLE; no_new_privs blocking setid
  M10     raw block device reads (/dev/sda) denied without the installer
          root bypass, for non-root and for installed-system root
  M17     forged SCM_CREDENTIALS over a unix socket rejected
  M1(c)   case-variant path impersonation: authority rides on the inode's
          role gid (phase 5.7 equivalent replacement, 09-m1-authority-design-
          review.md), so an unmarked copy at a look-alike path must not be
          tagged; the check keeps its Xfail reporting path as the standing
          rule if a grant ever reappears

Observables: every check runs a tiny probe ELF (tools/tests/
privilege_negatives_probe.c, compiled with the ReliefOS musl SDK and injected
into the scratch disk) that prints "PR <tag> key=value" lines to /dev/ttyS0;
flags come from ReliefOS /proc/<pid>/status (LeonOSFlags), AT_SECURE from
getauxval(3). Killability is observed as kill(2) from the same uid succeeding
plus the process dying (waitpid / gone from /proc).

Only disposable media is used: the source image is copied (reflink/sparse)
into the output directory and the copy is modified via debugfs; the source is
never written.

Usage:
    python3 tools/test_privilege_negatives.py --output /tmp/privneg-run
    python3 tools/test_privilege_negatives.py --output /tmp/run --image X.raw

Each named check prints "ok - <name>: ...", "FAIL - <name>: ...",
"xfail - known weakness: case-variant path impersonation (03-permission-matrix
M1(c)) ..." or "UNVERIFIED - <name>: ...". The exit code reflects non-xfail
failures only.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from test_installer_accounts_qemu import boot
from test_installer_window_qemu import wait_log
from test_vt_qemu import wait_text

ROOT = Path(__file__).resolve().parents[1]

# TASK_FLAG_SERVICE | TASK_FLAG_WINDOW_SERVER (kernel/reliefnt/include/reliefnt/sched.h).
SERVICE_BITS = 0x1 | 0x8

DESKTOP_DIR = "/usr/lib/reliefos/apps/desktop"
DESKTOP_PATH = f"{DESKTOP_DIR}/desktop.elf"
# M1 grants on the image inode's role gid (kernel RELIEFOS_GID_*), never on the
# path name (03-permission-matrix M1(c) fixed in phase 5.7). Fixture isolation:
# the writable copy carries the role gid so MODE is the only rejection reason,
# the case-variant copy is explicitly unmarked (gid 0) so it must stay inert.
WINDOW_SERVER_GID = 60001
WRITABLE_COPY = f"{DESKTOP_DIR}/Desktop.elf"    # user-writable copy (mode 0777)
CASEVARIANT_COPY = f"{DESKTOP_DIR}/DESKTOP.ELF"  # root-owned 0755, unmarked

M14_UNVERIFIED_REASON = (
    "not assertable in-guest today: kill(2)/signal delivery has no "
    "TASK_FLAG_SERVICE gate (kernel/reliefnt/kernel/signal/signal.c kernel_signal_queue_task_info, "
    "kernel/reliefnt/kernel/syscall/process/process.c LINUX_SYS_KILL); the documented refusal "
    "'kill windowd -> -1' lives only in sched_kill_user_task "
    "(kernel/reliefnt/kernel/sched/signal.c), whose sole caller "
    "auth_kill_session_tasks_for_logout (kernel/reliefnt/kernel/syscall/process/session.c, F3) is "
    "dead code, and sched_kill_user_tasks_for_pty/_for_logout have no callers. "
    "Impersonator killability is pinned by the m1-* checks instead.")


class Xfail(Exception):
    """Known current weakness: report as xfail, never fail the suite."""


class Guest:
    def __init__(self, probe, serial, process):
        self.probe = probe
        self.serial = serial
        self.process = process

    def sh(self, command, needle, timeout=90):
        self.probe.text(command)
        self.probe.key("ret")
        wait_log(self.serial, needle, self.process, timeout=timeout)

    def pr(self, tag):
        rows = []
        pattern = re.compile(rf"^PR {re.escape(tag)} (.+)$", re.M)
        for match in pattern.finditer(self.serial.read_text(errors="replace")):
            fields = {}
            for pair in match.group(1).split():
                key, _, value = pair.partition("=")
                fields[key] = value
            rows.append(fields)
        return rows

    def one(self, tag):
        rows = self.pr(tag)
        if not rows:
            raise AssertionError(f"no 'PR {tag}' line in serial log")
        # Some modes emit two lines with the same tag (result fields after the
        # child report); merge them into one record.
        merged = {}
        for row in rows:
            merged.update(row)
        return merged


def num(fields, key):
    return int(fields[key], 0)


def login(probe, process, tag, user, password):
    probe.text(user)
    probe.key("ret")
    wait_text(probe, process, f"{tag}-password", "Password:")
    probe.text(password)
    probe.key("ret")
    wait_text(probe, process, f"{tag}-shell", "built-in shell")


def login_root(probe, process):
    login(probe, process, "tty2", "root", "root")


def login_user(probe, process):
    login(probe, process, "tty3", "test", "test")


def untagged(fields, label, key="flags"):
    flags = num(fields, key)
    assert flags & SERVICE_BITS == 0, f"{label} has service bits LeonOSFlags={flags:#x}"
    return flags


def tagged(fields, label):
    flags = num(fields, "flags")
    assert flags & SERVICE_BITS == SERVICE_BITS, (
        f"{label} lacks SERVICE|WINDOW_SERVER LeonOSFlags={flags:#x}")
    return flags


# --------------------------------------------------------------------------
# Checks (each returns a short detail string, or raises AssertionError/Xfail)
# --------------------------------------------------------------------------

def check_m1b_root_userwritable_copy(guest):
    """M1(b): root exec of a user-writable copy at a service-looking path.

    The copy carries the role gid, so the image's group/world-writable bit is
    the only reason for the refusal (condition isolation).
    """
    guest.sh(f"{WRITABLE_COPY} flags m1b", "PR m1b ")
    flags = untagged(guest.one("m1b"), "root exec of user-writable copy")
    return f"LeonOSFlags={flags:#x} (no SERVICE for mode&0022 image)"


def check_m1c_case_variant(guest):
    """M1(c): root-owned non-writable case-variant copy must NOT be tagged.

    Authority rides on the inode's role gid (phase 5.7), so an unmarked copy is
    inert no matter what its path looks like. The Xfail path is retained as the
    standing rule if the behavior regresses to a grant.
    """
    guest.sh(f"{CASEVARIANT_COPY} flags m1c", "PR m1c ")
    fields = guest.one("m1c")
    flags = num(fields, "flags")
    if flags & SERVICE_BITS:
        raise Xfail(f"case-variant exec still granted LeonOSFlags={flags:#x}")
    return f"LeonOSFlags={flags:#x} (no grant: unmarked inode, path is not authority)"


def check_m10_root_rawdisk(guest):
    """M10: installed-system root (no installer root) reads /dev/sda."""
    guest.sh("/tmp/pp rd m10r /dev/sda", "PR m10r ")
    fields = guest.one("m10r")
    assert (num(fields, "open_errno") == 13 or num(fields, "read_errno") == 13), (
        f"raw disk read not denied: {fields}")
    return (f"open_errno={fields['open_errno']} read_errno={fields['read_errno']} "
            "(EACCES)")


def swap_probe_in(guest):
    """Put the probe at the exact desktop path (backup kept in /tmp).

    The role gid is re-applied explicitly so the positive control does not bet
    on cp(1) preserving the destination inode's gid.
    """
    guest.sh(f"cp {DESKTOP_PATH} /tmp/desktop-stock.elf && cp /tmp/pp {DESKTOP_PATH}"
             f" && chmod 0755 {DESKTOP_PATH} && chgrp {WINDOW_SERVER_GID} {DESKTOP_PATH}"
             f" && echo PRIV-SWAP-OK >/dev/ttyS0",
             "PRIV-SWAP-OK")


def restore_stock(guest):
    guest.sh(f"cp /tmp/desktop-stock.elf {DESKTOP_PATH}"
             f" && chmod 0755 {DESKTOP_PATH} && chgrp {WINDOW_SERVER_GID} {DESKTOP_PATH}"
             f" && echo PRIV-RESTORE-OK >/dev/ttyS0",
             "PRIV-RESTORE-OK")


def check_m1_positive_control(guest):
    """Positive control: root exec of the role-marked image (root-owned,
    non-writable, gid 60001) still receives
    TASK_FLAG_SERVICE|TASK_FLAG_WINDOW_SERVER (M1 grant)."""
    guest.sh(f"{DESKTOP_PATH} flags m1pos", "PR m1pos ")
    flags = tagged(guest.one("m1pos"), "exec at exact desktop path")
    return f"LeonOSFlags={flags:#x} (SERVICE|WINDOW_SERVER granted)"


def check_m3_fork_child(guest):
    """M3: clone strips SERVICE/WINDOW_SERVER; the fork child is killable."""
    guest.sh(f"{DESKTOP_PATH} forkp m3", "PR m3.result ")
    parent = tagged(guest.one("m3"), "service parent")
    child = untagged(guest.one("m3.child"), "fork child")
    result = guest.one("m3.result")
    assert num(result, "kill_rc") == 0, f"kill of fork child failed: {result}"
    assert num(result, "dead") == 1, f"fork child survived kill: {result}"
    return (f"parent LeonOSFlags={parent:#x} child LeonOSFlags={child:#x} "
            "child killed and reaped")


def check_m2_exec_clears_flags(guest):
    """M2: exec keeps only ELEVATED_ADMIN|WAITABLE_CHILD; service bits drop."""
    guest.sh(f"{DESKTOP_PATH} reexec m2 /tmp/pp", "PR m2.after ")
    before = tagged(guest.one("m2"), "pre-exec service process")
    after = untagged(guest.one("m2.after"), "post-exec image")
    return f"LeonOSFlags {before:#x} -> {after:#x} across exec"


def check_m1a_shell_killable(guest):
    """M1(a)/M14 negative, shell observable: the ordinary user runs the real
    desktop path directly, sees no service bits in /proc, kills the process
    from the same user and it dies."""
    guest.sh(f"{DESKTOP_PATH} hang m1a.shell 60 &", "PR m1a.shell ")
    self_flags = untagged(guest.one("m1a.shell"), "non-root exec at real path")
    guest.sh("echo M1A-S-START >/dev/ttyS0 ; grep LeonOSFlags /proc/$!/status"
             " >/dev/ttyS0 ; echo M1A-S-END >/dev/ttyS0", "M1A-S-END")
    guest.sh("kill $! && echo M1A-S-KILL-OK >/dev/ttyS0"
             " || echo M1A-S-KILL-FAIL >/dev/ttyS0", "M1A-S-KILL")
    guest.sh("wait ; test -e /proc/$! && echo M1A-S-ALIVE >/dev/ttyS0"
             " || echo M1A-S-DEAD >/dev/ttyS0 ; echo M1A-S-DONE >/dev/ttyS0",
             "M1A-S-DONE")
    text = guest.serial.read_text(errors="replace")
    assert "M1A-S-KILL-OK" in text, "same-user kill did not succeed"
    assert "M1A-S-KILL-FAIL" not in text, "same-user kill reported failure"
    assert "M1A-S-DEAD" in text, "process still present after kill"
    assert "M1A-S-ALIVE" not in text, "process survived same-user kill"
    region = text.split("M1A-S-START", 1)[-1].split("M1A-S-END", 1)[0]
    match = re.search(r"LeonOSFlags:\s+(\d+)", region)
    assert match, f"no LeonOSFlags in /proc status output: {region!r}"
    proc_flags = int(match.group(1))
    assert proc_flags & SERVICE_BITS == 0, (
        f"/proc reports service bits LeonOSFlags={proc_flags:#x}")
    return (f"self LeonOSFlags={self_flags:#x} /proc LeonOSFlags={proc_flags:#x} "
            "same-user kill ok, process dead")


def check_m1_nonroot_userwritable_copy(guest):
    """M1/M14: the ordinary user execs a user-writable copy at a
    service-looking path - no service bits, killable from the same user."""
    guest.sh(f"/tmp/pp imp m1w 1000 {WRITABLE_COPY} hang m1w.child 60",
             "PR m1w.result ")
    result = guest.one("m1w.result")
    assert result.get("child_name") == "Desktop.elf", (
        f"exec of the user-writable copy did not happen: {result}")
    flags = untagged(result, "non-root exec of user-writable copy", key="child_flags")
    child = guest.one("m1w.child")
    untagged(child, "impersonating child self-report")
    assert num(result, "kill_rc") == 0, f"same-user kill failed: {result}"
    assert num(result, "dead") == 1, f"impersonator survived kill: {result}"
    return (f"child LeonOSFlags={flags:#x} child_uid={result['child_uid']} "
            "same-user kill ok, process dead")


def check_m4_setuid_at_secure(guest):
    """M4: setuid-root exec by a non-root user yields AT_SECURE semantics.

    Observable: getauxval(AT_SECURE) + uid/euid from the auxv-printing probe
    (no auxv tool exists in the image, so the probe ELF is the helper) +
    PR_GET_DUMPABLE; the plain exec is the contrast.
    """
    guest.sh("/tmp/pp-suid flags m4", "PR m4 ")
    elevated = guest.one("m4")
    assert num(elevated, "uid") == 1000, f"real uid changed: {elevated}"
    assert num(elevated, "euid") == 0, f"setuid euid transition missing: {elevated}"
    assert num(elevated, "atsecure") == 1, f"AT_SECURE not set: {elevated}"
    assert num(elevated, "dumpable") == 0, f"setuid exec stayed dumpable: {elevated}"
    guest.sh("/tmp/pp flags m4plain", "PR m4plain ")
    plain = guest.one("m4plain")
    assert num(plain, "euid") == 1000, f"plain exec raised euid: {plain}"
    assert num(plain, "atsecure") == 0, f"plain exec set AT_SECURE: {plain}"
    return (f"setuid: uid={elevated['uid']} euid={elevated['euid']} "
            f"atsecure={elevated['atsecure']} dumpable={elevated['dumpable']}; "
            f"plain: euid={plain['euid']} atsecure={plain['atsecure']}")


def check_m4_no_new_privs(guest):
    """M4: no_new_privs blocks the setid transition (and AT_SECURE)."""
    guest.sh("/tmp/pp nnp m4n /tmp/pp-suid", "PR m4n.after ")
    fields = guest.one("m4n.after")
    assert num(fields, "euid") == 1000, f"no_new_privs allowed setid: {fields}"
    assert num(fields, "atsecure") == 0, f"blocked setid set AT_SECURE: {fields}"
    return f"euid={fields['euid']} atsecure={fields['atsecure']} (setid blocked)"


def check_m10_nonroot_rawdisk(guest):
    """M10: non-root read of the raw disk device node fails with EACCES."""
    guest.sh("/tmp/pp rd m10u /dev/sda", "PR m10u ")
    fields = guest.one("m10u")
    assert (num(fields, "open_errno") == 13 or num(fields, "read_errno") == 13), (
        f"non-root raw disk read not denied: {fields}")
    return (f"open_errno={fields['open_errno']} read_errno={fields['read_errno']} "
            "(EACCES)")


def check_m17_scm_credentials(guest):
    """M17: a forged uid in SCM_CREDENTIALS is rejected with EPERM."""
    guest.sh("/tmp/pp scm m17", "PR m17.real ")
    forged = guest.one("m17.forged")
    real = guest.one("m17.real")
    assert num(forged, "rc") == -1 and num(forged, "errno") == 1, (
        f"forged SCM_CREDENTIALS not rejected with EPERM: {forged}")
    assert num(real, "rc") >= 0, f"legitimate SCM_CREDENTIALS failed: {real}"
    return (f"forged rc={forged['rc']} errno={forged['errno']} (EPERM); "
            f"real rc={real['rc']}")


def check_m1a_stock_real_path(guest):
    """M1(a): the ordinary user runs the REAL desktop.elf binary at the real
    service path directly - no service bits, process killable and dead."""
    guest.sh(f"/tmp/pp imp m1stock 1000 {DESKTOP_PATH}", "PR m1stock.result ")
    result = guest.one("m1stock.result")
    assert result.get("child_name") == "desktop.elf", (
        f"exec of the real desktop.elf did not happen: {result}")
    flags = untagged(result, "non-root exec of real desktop.elf", key="child_flags")
    assert num(result, "dead") == 1, f"real desktop.elf survived kill: {result}"
    assert num(result, "kill_rc") == 0, f"kill of real desktop.elf failed: {result}"
    return (f"child_name={result['child_name']} child_uid={result['child_uid']} "
            f"child_flags={flags:#x} killed and dead")


# --------------------------------------------------------------------------
# Harness
# --------------------------------------------------------------------------

def locate_image(explicit):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("PRIVNEG_IMAGE"):
        candidates.append(Path(os.environ["PRIVNEG_IMAGE"]))
    candidates.append(Path("/home/leon/build/ntclks-sep/p4-out/images/leonos4.raw"))
    candidates.append(ROOT / "out/x86_64/release/images/leonos4.raw")
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise SystemExit(f"no ReliefOS image found; tried: {candidates}")


def locate_compiler():
    candidates = []
    if os.environ.get("PRIVNEG_CC"):
        candidates.append(Path(os.environ["PRIVNEG_CC"]))
    candidates.append(ROOT / "out/x86_64/release/sdk/leonos-musl-sdk/bin/reliefos-musl-cc")
    candidates.append(Path("/home/leon/build/ntclks-sep/p4-out/sdk/leonos-musl-sdk/bin/reliefos-musl-cc"))
    candidates.append(Path("/home/leon/build/ntclks-sep/p4-out/host/bin/reliefos-musl-cc"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit(f"no reliefos-musl-cc found; tried: {candidates}")


def build_probe(output):
    compiler = locate_compiler()
    binary = output / "privprobe"
    subprocess.run([str(compiler), "-D_GNU_SOURCE", "-static", "-O2",
                    str(ROOT / "tools/tests/privilege_negatives_probe.c"),
                    "-o", str(binary)], check=True)
    return binary


def extract_root_partition(disk, target):
    layout = json.loads(subprocess.check_output(["sfdisk", "--json", str(disk)]))["partitiontable"]
    root = layout["partitions"][1]
    offset, length = root["start"] * layout["sectorsize"], root["size"] * layout["sectorsize"]
    with disk.open("rb") as source, target.open("wb") as destination:
        source.seek(offset)
        remaining = length
        while remaining:
            chunk = source.read(min(16 * 1024**2, remaining))
            if not chunk:
                raise RuntimeError("truncated root partition")
            destination.write(chunk)
            remaining -= len(chunk)
    return offset


def prepare_scratch(image, output, probe):
    """Disposable copy + debugfs-injected probe files; the image is read-only."""
    disk = output / "scratch.raw"
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(image), str(disk)],
                   check=True)
    filesystem = output / "root.ext2"
    offset = extract_root_partition(disk, filesystem)
    commands = [
        f"write {probe} /tmp/pp",
        "set_inode_field /tmp/pp mode 0100755",
        "set_inode_field /tmp/pp uid 0",
        "set_inode_field /tmp/pp gid 0",
        f"write {probe} /tmp/pp-suid",
        "set_inode_field /tmp/pp-suid mode 0104755",
        "set_inode_field /tmp/pp-suid uid 0",
        "set_inode_field /tmp/pp-suid gid 0",
        f"write {probe} {WRITABLE_COPY}",
        "set_inode_field %s mode 0100777" % WRITABLE_COPY,
        "set_inode_field %s uid 0" % WRITABLE_COPY,
        # Role gid on the writable copy: writability is the only rejection gate.
        "set_inode_field %s gid %d" % (WRITABLE_COPY, WINDOW_SERVER_GID),
        f"write {probe} {CASEVARIANT_COPY}",
        "set_inode_field %s mode 0100755" % CASEVARIANT_COPY,
        "set_inode_field %s uid 0" % CASEVARIANT_COPY,
        # Explicitly unmarked (gid 0): an untagged copy must stay inert.
        "set_inode_field %s gid 0" % CASEVARIANT_COPY,
    ]
    for command in commands:
        subprocess.run(["debugfs", "-w", "-R", command, str(filesystem)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with filesystem.open("rb") as source, disk.open("r+b") as destination:
        destination.seek(offset)
        shutil.copyfileobj(source, destination, 16 * 1024**2)
    return disk


def run_checks(guest, results):
    def record(name, function):
        try:
            detail = function(guest)
        except Xfail as exception:
            print(f"xfail - known weakness: case-variant path impersonation "
                  f"(03-permission-matrix M1(c)) [{name}: {exception}]", flush=True)
            results.append((name, "xfail"))
        except AssertionError as exception:
            print(f"FAIL - {name}: {exception}", flush=True)
            results.append((name, "FAIL"))
        else:
            print(f"ok - {name}: {detail}", flush=True)
            results.append((name, "ok"))
        try:
            guest.probe.frame(f"check-{name}")
        except Exception:  # screenshots are evidence, never verdicts
            pass

    # Root session first: the stock desktop.elf must stay intact for the
    # final check, and the probe swap only happens after the early checks.
    record("m1-root-userwritable-copy-no-grant", check_m1b_root_userwritable_copy)
    record("m1c-case-variant-path-impersonation", check_m1c_case_variant)
    record("m10-root-no-installer-rawdisk-eacces", check_m10_root_rawdisk)
    swap_probe_in(guest)
    record("m1-service-grant-positive-control", check_m1_positive_control)
    record("m3-service-fork-child-stripped-killable", check_m3_fork_child)
    record("m2-exec-clears-service-flags", check_m2_exec_clears_flags)

    # Ordinary-user session (test, uid 1000) on tty3.
    guest.probe.key("ctrl-alt-f3")
    wait_text(guest.probe, guest.process, "tty3-login", "login:")
    login_user(guest.probe, guest.process)
    record("m1-nonroot-real-path-untagged-killable", check_m1a_shell_killable)
    record("m1-nonroot-userwritable-copy-untagged-killable",
           check_m1_nonroot_userwritable_copy)
    record("m4-setuid-at-secure", check_m4_setuid_at_secure)
    record("m4-no-new-privs-blocks-setid", check_m4_no_new_privs)
    record("m10-nonroot-rawdisk-eacces", check_m10_nonroot_rawdisk)
    record("m17-scm-credentials-forge-eperm", check_m17_scm_credentials)

    # Back to root: restore the stock binary, then run it (last, because the
    # real desktop may disturb the active VT) as the ordinary user.
    guest.probe.key("ctrl-alt-f2")
    restore_stock(guest)
    record("m1-nonroot-real-path-exec-untagged", check_m1a_stock_real_path)

    print(f"UNVERIFIED - m14-service-kill-refusal: {M14_UNVERIFIED_REASON}", flush=True)
    results.append(("m14-service-kill-refusal", "unverified"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory for scratch disk, serial log and screenshots")
    parser.add_argument("--image", type=Path,
                        help="ReliefOS disk image to copy (default: p4-out/out build)")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)

    image = locate_image(args.image)
    print(f"image: {image}", flush=True)
    probe = build_probe(output)
    disk = prepare_scratch(image, output, probe)

    results = []
    with boot(output, disk) as (qemu_probe, serial, process):
        guest = Guest(qemu_probe, serial, process)
        wait_log(serial, "boot complete:", process, timeout=150)
        # The tty1 graphical session activates its VT once ready and would
        # yank back an early Ctrl+Alt+F2; wait for it first (test_vt_qemu).
        wait_text(qemu_probe, process, "tty1-login-ui", "root", timeout=90)
        qemu_probe.key("ctrl-alt-f2")
        wait_text(qemu_probe, process, "tty2-login", "login:", timeout=90)
        login_root(qemu_probe, process)
        try:
            run_checks(guest, results)
        finally:
            try:
                qemu_probe.frame("last-frame")
            except Exception:
                pass

    counts = {status: sum(1 for _, s in results if s == status)
              for status in ("ok", "xfail", "unverified", "FAIL")}
    print(f"SUMMARY: ok={counts['ok']} xfail={counts['xfail']} "
          f"unverified={counts['unverified']} fail={counts['FAIL']}", flush=True)
    sys.exit(1 if counts["FAIL"] else 0)


if __name__ == "__main__":
    main()
