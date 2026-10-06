#!/usr/bin/env python3
"""Boot the Xorg image on the VMware SVGA device and verify the tty1 session.

The scenario is the acceptance walk-through from the desktop task: tty1 runs the
XDM/Xorg/TWM graphical session, tty2-tty6 stay text logins that accept typed
input, Ctrl+Alt+Fn switches the display both ways, and the guest's XDM lifecycle
record is captured as the evidence log that tools/test_xorg_qemu.py validates.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import time

from test_installer_accounts_qemu import Probe
from test_installer_window_qemu import wait_log
from test_vt_qemu import ocr, wait_text
from run_gcc_probe_qemu import qmp_quit

ROOT = Path(__file__).resolve().parents[1]
IMAGE = Path(os.environ.get("XORG_IMAGE",
                            ROOT / "out/xorg-vmware-debug/images/reliefos.raw"))
OUTPUT = Path(os.environ.get("XORG_OUTPUT", ROOT / "build/xorg-vm"))


def boot(process_args, disk, output):
    """Like test_installer_accounts_qemu.boot, but on the VMware SVGA device."""
    output.mkdir(parents=True, exist_ok=True)
    serial, qmp = output / "serial.log", output / "qmp.sock"
    command = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-machine", "q35",
               "-m", "4096", "-smp", "2", "-bios", "/usr/share/edk2/x64/OVMF.4m.fd",
               "-display", "none", "-serial", f"file:{serial}",
               "-vga", "vmware",
               "-device", "qemu-xhci", "-device", "usb-tablet",
               "-netdev", "user,id=net0", "-device", "e1000,netdev=net0",
               "-drive", f"file={disk},format=raw,if=ide,snapshot=on",
               "-qmp", f"unix:{qmp},server=on,wait=off", "-no-reboot", "-no-shutdown",
               *process_args]
    (output / "qemu.log").write_text("")
    return subprocess.Popen(command, stdout=(output / "qemu.log").open("w"),
                            stderr=subprocess.STDOUT, cwd=ROOT), serial, qmp


def serial_run(probe, serial, process, command, needle, timeout=30):
    """Type a command on tty2's shell and wait for its serial marker."""
    offset = len(serial.read_text(errors="replace")) if serial.exists() else 0
    probe.text(command)
    probe.key("ret")
    wait_serial_after(serial, needle, process, offset, timeout=timeout)


def login(probe, process, name, user, password, prompt_needle):
    wait_text(probe, process, f"{name}-prompt", prompt_needle)
    probe.text(user)
    probe.key("ret")
    wait_text(probe, process, f"{name}-password", "Password:")
    probe.text(password)
    probe.key("ret")
    wait_text(probe, process, f"{name}-shell", "built-in shell")


GREETER_NEEDLE = "unsecure"   # xlogin's banner; gone once a session runs


def classify_greeter_text(text):
    """Classify one panel OCR result without treating missing OCR as progress."""
    normalized = re.sub(r"[^a-z0-9:]+", " ", text.lower())
    if re.search(r"\bpassword\s*:", normalized):
        return "password"
    if re.search(r"\blogin\s*:", normalized):
        return "login"
    if GREETER_NEEDLE not in normalized:
        return "session"
    return "unknown"


def _bright_panel_bbox(frame):
    """Find xlogin's large white panel, ignoring bright terminal glyphs."""
    width, height = frame.size
    pixels = frame.load()

    def bright(pixel):
        return min(pixel[:3]) >= 220

    # A bright wallpaper band (the pale sky) is bright too, but it runs edge
    # to edge.  Only rows whose widest bright run keeps a margin on both
    # sides can belong to the centered panel.
    edge = width * 0.08
    row_hits = []
    for y in range(height):
        if sum(bright(pixels[x, y]) for x in range(width)) < width * 0.40:
            continue
        runs = []
        start = None
        for x in range(width):
            if bright(pixels[x, y]):
                if start is None:
                    start = x
            elif start is not None:
                runs.append((start, x))
                start = None
        if start is not None:
            runs.append((start, width))
        left, right = max(runs, key=lambda run: run[1] - run[0], default=(0, 0))
        if left >= edge and right <= width - edge:
            row_hits.append(y)
    if len(row_hits) < max(20, height // 20):
        return None
    # Text and the separator split individual rows.  Group rows into bands
    # tolerating short gaps, then aggregate each band so a single row cannot
    # select only half of the white surface.  The panel is the largest band.
    gap = max(4, height // 100)
    bands = [[row_hits[0]]]
    for y in row_hits[1:]:
        if y - bands[-1][-1] <= gap:
            bands[-1].append(y)
        else:
            bands.append([y])
    best = None
    for band in bands:
        if len(band) < max(20, height // 20):
            continue
        min_hits = int(len(band) * .70)
        xs = [x for x in range(width)
              if sum(bright(pixels[x, y]) for y in band) >= min_hits]
        x_runs = []
        for x in xs:
            if not x_runs or x != x_runs[-1][-1] + 1:
                x_runs.append([x])
            else:
                x_runs[-1].append(x)
        columns = max(x_runs, key=len, default=[])
        if len(columns) < width * 0.40:
            continue
        # xlogin's panel is centered with a margin on every side.  A running
        # xterm is also a bright rectangle, but it touches the display edge
        # and must not keep the session probe in the greeter state.
        if (columns[0] < width * 0.10 or columns[-1] >= width * 0.90 or
                band[0] < height * 0.10 or band[-1] >= height * 0.90):
            continue
        bbox = (columns[0], band[0], columns[-1] + 1, band[-1] + 1)
        area = (bbox[2] - bbox[0]) * (bbox[3] - bbox[1])
        if best is None or area > best[0]:
            best = (area, bbox)
    return best[1] if best else None


def greeter_ocr(frame):
    """OCR only xlogin's panel and label area, avoiding the surrounding VT."""
    bbox = _bright_panel_bbox(frame)
    if bbox is None:
        return ""
    panel = frame.crop(bbox)
    # psm 11 keeps the two labels as separate sparse text.  The label crop is
    # a fallback for the serif Password label, which psm 6 often drops.
    text = ocr(panel, psm=11)
    left = panel.crop((int(panel.width * .08), int(panel.height * .35),
                       int(panel.width * .36), int(panel.height * .78)))
    return text + "\n" + ocr(left, psm=6)


def greeter_stage(frame):
    """Return login/password/session/unknown from an auditable frame crop."""
    return classify_greeter_text(greeter_ocr(frame))


def lifecycle_events(log):
    """Extract only the authenticated non-root session lifecycle events."""
    patterns = (
        ("PAM authentication accepted", re.compile(r"PAM authentication accepted\b")),
        ("twm started for uid", re.compile(r"twm started for uid=(?!0\b)[1-9][0-9]*\b")),
        ("xterm started", re.compile(r"xterm started\b")),
        ("xdm session ended", re.compile(r"xdm session ended\b")),
        ("tty1 restored to text login", re.compile(r"tty1 restored to text login\b")),
    )
    matches = []
    for name, pattern in patterns:
        match = pattern.search(log)
        if match:
            matches.append((match.start(), name))
    return [name for _, name in sorted(matches)]


def require_event_order(log):
    """Require the authenticated XDM lifecycle to be recorded in order."""
    expected = [
        "PAM authentication accepted", "twm started for uid",
        "xterm started", "xdm session ended", "tty1 restored to text login",
    ]
    actual = lifecycle_events(log)
    if actual != expected:
        raise AssertionError(f"invalid XDM lifecycle order: {actual!r}")


def require_vt_handshake(serial_text):
    """Require the kernel VT_PROCESS release/acquire trace for a real switch."""
    required = (
        "VT mode vt=1 mode=1",
        "VT graphics vt=1 enabled=1",
        "VT switch request 1 -> 2",
        "VT release signal sent for vt=1",
        "VT active 2 graphics=0",
        "VT switch request 2 -> 1",
        "VT active 1 graphics=1",
        "VT acquire signal sent for vt=1",
    )
    cursor = 0
    for needle in required:
        position = serial_text.find(needle, cursor)
        if position < 0:
            raise AssertionError(f"missing or out-of-order VT trace: {needle}")
        cursor = position + len(needle)


def wait_serial_after(serial, needle, process, offset, timeout=30):
    """Wait for a marker emitted after ``offset``; stale logs cannot satisfy it."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial.read_text(errors="replace") if serial.exists() else ""
        if needle in text[offset:]:
            return
        if process.poll() is not None:
            raise RuntimeError("QEMU exited unexpectedly")
        time.sleep(.2)
    raise AssertionError(f"Missing serial output after current action: {needle}")


def greeter_login(probe, process):
    """Sign in at the XDM xlogin greeter: name first, then the password."""
    wait_greeter_stage(probe, process, "login", "tty1-greeter")
    print("xorg-vm: greeter:", repr(greeter_ocr(probe.frame("tty1-greeter-detail"))[:400]), flush=True)
    # Re-focus after a VT acquire.  XDM may redraw before Xorg has delivered
    # the first post-acquire pointer/keyboard event to the input widget.
    probe.click(700, 310)
    probe.text("test")
    probe.key("ret")            # accept the name; xlogin swaps in the password
    wait_greeter_stage(probe, process, "password", "tty1-greeter-password")
    # xlogin keeps the password field at the same y coordinate as the login
    # field; clicking below the field leaves the keyboard focus on the root
    # window and submits an empty password, returning to the greeter.
    probe.click(700, 310)
    probe.text("test")
    probe.key("ret")            # submit


def wait_greeter_stage(probe, process, expected, name, timeout=45):
    """Wait for a specific xlogin stage using a panel-scoped OCR observation."""
    deadline = time.monotonic() + timeout
    observed = ""
    stable = 0
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited during login"
        frame = probe.frame(name)
        observed = greeter_stage(frame)
        if observed == expected:
            stable += 1
            if stable >= 2:
                return
        else:
            stable = 0
        time.sleep(.5)
    raise AssertionError(f"XDM greeter did not reach {expected!r}; observed {observed!r}")


def _session_xterm_visible(frame):
    """Return True once the session xterm has mapped its window.

    The pale wallpaper alone already passes any brightness floor on the bare
    root window the moment xdm kills the greeter, long before twm/xterm have
    mapped.  The xterm started by xdm-session fills its window with #f7f8fb
    and the wallpaper palette contains no pixel of that exact color, so only
    a mapped terminal can put it on screen -- the readiness signal needed
    before typing the marker command.
    """
    colors = frame.getcolors(1 << 20) or []
    return sum(count for count, rgb in colors if rgb == (247, 248, 251)) >= 20000


def wait_session(probe, process, name, timeout=45):
    """Require a mapped session window before sending an xterm probe."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        assert process.poll() is None, "QEMU exited while waiting for the session"
        frame = probe.frame(name)
        if (_bright_panel_bbox(frame) is None and
                _session_xterm_visible(frame)):
            return
        time.sleep(.5)
    raise AssertionError(f"TWM/xterm session never became visible on {name}")


def xterm_marker(probe, serial, process, marker, command=None):
    """Prove that input reached the xterm shell by observing its serial marker."""
    command = command or f"echo {marker} >/dev/ttyS0"
    offset = len(serial.read_text(errors="replace")) if serial.exists() else 0
    # TWM keeps the keyboard focus with the pointer, and the greeter login
    # clicks leave the pointer outside the session xterm that xdm-session
    # places at +0+0.  Click the terminal first, the same way greeter_login
    # focuses the login field before typing.
    probe.click(300, 200)
    probe.text(command)
    probe.key("ret")
    wait_serial_after(serial, marker, process, offset, timeout=20)


def switch_vt(probe, serial, process, number):
    """Switch and wait for the kernel's active-VT trace, not a fixed sleep."""
    offset = len(serial.read_text(errors="replace")) if serial.exists() else 0
    probe.key(f"ctrl-alt-f{number}")
    wait_serial_after(serial, f"VT active {number} graphics={'1' if number == 1 else '0'}",
                      process, offset, timeout=20)
    if number == 1:
        # The active trace precedes the VT_PROCESS acquire signal.  Xorg can
        # redraw the saved framebuffer before its input fds are resumed, so
        # wait for that handoff and let the signal handler settle before typing.
        wait_serial_after(serial, "VT acquire signal sent for vt=1",
                          process, offset, timeout=20)
    # Let the console's input path settle on every switch: typing straight
    # after the active trace can drop individual keys.
    time.sleep(1.0)


def frame_distance(first, second):
    """Return normalized grayscale distance for repaint/input-isolation evidence."""
    a = first.convert("L").resize((160, 90))
    b = second.convert("L").resize((160, 90))
    return sum(abs(a.getpixel((x, y)) - b.getpixel((x, y)))
               for y in range(90) for x in range(160)) / (160 * 90 * 255)


def collect(probe, serial, process, tag, path=None, command=None):
    """Run a guest command and read its output between serial markers."""
    if (path is None) == (command is None):
        raise ValueError("exactly one of path or command is required")
    # QEMU's serial file is buffered independently of the QMP command.  A
    # length snapshot can advance past a marker that was already emitted but
    # not flushed, making a valid response look stale.  A unique marker lets
    # us search the full stream and remains unambiguous when the host flushes
    # serial output out of order.
    nonce = f"{time.monotonic_ns():x}"
    begin, end = f"=== {tag}-{nonce} BEGIN ===", f"=== {tag}-{nonce} END ==="
    body = command or f"cat {path}"
    # Keep the injected shell syntax within the Probe key map: braces and
    # printf's percent sign are layout-sensitive, while echo and semicolons
    # are stable on the guest's busybox shell.
    probe.text(f"echo '{begin}' >/dev/ttyS0; {body} >/dev/ttyS0; echo '{end}' >/dev/ttyS0")
    probe.key("ret")
    wait_serial_after(serial, end, process, 0, timeout=20)
    text = serial.read_text(errors="replace")
    start = text.rfind(begin)
    finish = text.find(end, start + len(begin))
    if start < 0 or finish < 0:
        raise AssertionError(f"serial markers incomplete for {tag}")
    return text[start + len(begin):finish].strip("\n")


def fresh_log_suffix(before, after, label):
    """Ensure the final root log extends the pre-session snapshot."""
    if not after.startswith(before):
        raise AssertionError(f"{label} changed prefix during the run")
    return after[len(before):]


def main() -> int:
    if not IMAGE.is_file():
        print(f"xorg-vm: image not built: {IMAGE}")
        return 1
    shutil.rmtree(OUTPUT, ignore_errors=True)
    OUTPUT.mkdir(parents=True)
    failures = []

    def fail(message):
        print(f"xorg-vm: FAIL {message}", flush=True)
        failures.append(message)

    process, serial, qmp = boot([], IMAGE, OUTPUT)
    probe = None
    session_ready = False
    xdm_before = ""
    xdm_log = ""
    xdm_delta = ""
    xorg_log = ""
    try:
        deadline = time.monotonic() + 15
        while not qmp.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before QMP became ready")
            time.sleep(.1)
        probe = Probe(qmp, OUTPUT)
        wait_log(serial, "boot complete:", process, timeout=120)
        print("xorg-vm: boot complete", flush=True)

        # The greeter stage is scoped to the large white xlogin panel.  This
        # avoids the permanent wait caused by full-screen OCR missing the
        # serif Password label, while retaining the screenshot as evidence.
        wait_greeter_stage(probe, process, "login", "tty1-graphical", timeout=90)

        # Establish a root shell on tty2 before authentication so the final
        # XDM log can be compared with a byte-for-byte pre-session snapshot.
        # This prevents lifecycle records left in a prebuilt image from passing.
        switch_vt(probe, serial, process, 2)
        login(probe, process, "tty2-baseline", "root", "root", "login:")
        xdm_before = collect(probe, serial, process, "XDM-BASELINE", path="/var/log/xdm.log")
        switch_vt(probe, serial, process, 1)
        wait_greeter_stage(probe, process, "login", "tty1-greeter-return")

        try:
            greeter_login(probe, process)
            wait_session(probe, process, "tty1-session-live")
            xterm_marker(probe, serial, process, "XORG-XTERM-READY")
            session_ready = True
        except AssertionError as error:
            fail(f"tty1 session did not replace the greeter: {error}")
            # Keep an unsuccessful hand-off actionable.  The framebuffer only
            # shows that xlogin remained visible; the process table and Xorg
            # log distinguish a dead server from a live server whose VT/evdev
            # input path never resumed.  Collect through the same serial
            # marker protocol used by the passing path so partial output
            # cannot be mistaken for evidence.
            try:
                switch_vt(probe, serial, process, 2)
                failure_ps = collect(probe, serial, process,
                                     "GREETER-FAIL-PS", command="ps")
                failure_xorg = collect(probe, serial, process,
                                       "GREETER-FAIL-XORG", path="/var/log/Xorg.0.log")
                failure_xdm = collect(probe, serial, process,
                                      "GREETER-FAIL-XDM", path="/var/log/xdm.log")
                failure_session = collect(probe, serial, process,
                                          "GREETER-FAIL-XSESSION",
                                          path="/home/test/.xsession-errors")
                (OUTPUT / "greeter-failure-ps.txt").write_text(failure_ps + "\n")
                (OUTPUT / "greeter-failure-Xorg.0.log").write_text(failure_xorg + "\n")
                (OUTPUT / "greeter-failure-xdm.log").write_text(failure_xdm + "\n")
                (OUTPUT / "greeter-failure-xsession-errors").write_text(failure_session + "\n")
            except (AssertionError, RuntimeError) as diagnostic_error:
                fail(f"greeter failure diagnostics unavailable: {diagnostic_error}")

        if session_ready:
            # tty3-tty6 remain independent text terminals.  tty2 was already
            # authenticated above and its shell must survive every switch.
            for number in range(3, 7):
                switch_vt(probe, serial, process, number)
                try:
                    wait_text(probe, process, f"tty{number}-prompt", "login:", timeout=25)
                except AssertionError as error:
                    fail(f"tty{number} lost its login prompt: {error}")
            switch_vt(probe, serial, process, 2)
            serial_run(probe, serial, process,
                       "echo TTY2-INPUT-OK >/dev/ttyS0", "TTY2-INPUT-OK")
            serial_run(probe, serial, process,
                       "echo TTY2-TTY=$(tty) >/dev/ttyS0", "TTY2-TTY=/dev/tty2")

            # Type a sentinel on the active text VT without submitting it.
            # After the return, neither OCR nor the measurable framebuffer
            # distance may show that text in the xterm surface.
            switch_vt(probe, serial, process, 1)
            wait_session(probe, process, "tty1-before-isolation")
            xterm_marker(probe, serial, process, "XORG-RESTORED")
            baseline = probe.frame("tty1-isolation-baseline")
            switch_vt(probe, serial, process, 2)
            sentinel = "TTY2-ONLY-INPUT-9F3A"
            probe.text(sentinel)
            probe.frame("tty2-unsubmitted-sentinel")
            switch_vt(probe, serial, process, 1)
            restored = probe.frame("tty1-after-isolation")
            visible = (ocr(restored, psm=6) + "\n" + ocr(restored, psm=11)).lower()
            distance = frame_distance(baseline, restored)
            (OUTPUT / "input-isolation.txt").write_text(
                f"sentinel={sentinel}\nocr_contains={sentinel.lower() in visible}\n"
                f"frame_distance={distance:.6f}\n")
            if sentinel.lower() in visible:
                fail("tty2 sentinel leaked into the graphical session")
            switch_vt(probe, serial, process, 2)
            probe.key("ctrl-c")
            serial_run(probe, serial, process,
                       "echo TTY2-AFTER-ISOLATION >/dev/ttyS0", "TTY2-AFTER-ISOLATION")

            # Capture the real process table, then terminate TWM through the
            # xterm shell.  This exercises XDM's session-ended and tty1 getty
            # handoff paths instead of leaving the session alive at teardown.
            processes = collect(probe, serial, process, "SESSION-PROCS", command="ps")
            twm = re.search(r"(?im)^\s*(\d+)\s+.*\btwm(?:\s|$)", processes)
            if re.search(r"(?im)(?:getty|login\.elf).*tty1\b", processes):
                fail(f"text getty/login still owns tty1 during the graphical session: {processes!r}")
            if not twm:
                fail(f"session process table has no TWM pid: {processes!r}")
            else:
                twm_pid = twm.group(1)
                switch_vt(probe, serial, process, 1)
                wait_session(probe, process, "tty1-before-exit")
                xterm_marker(probe, serial, process, "XORG-EXIT-REQUEST",
                             f"echo XORG-EXIT-REQUEST >/dev/ttyS0; kill {twm_pid}")
                # XDM may immediately present a fresh greeter after the user
                # session ends.  First collect that end event, then terminate
                # the display manager itself to exercise tty1's text fallback.
                switch_vt(probe, serial, process, 2)
                session_log = collect(probe, serial, process,
                                      "XDM-AFTER-SESSION", path="/var/log/xdm.log")
                if "xdm session ended" not in session_log:
                    fail("XDM log has no session-ended event after TWM termination")
                xdm_ps = collect(probe, serial, process, "XDM-PROCS", command="ps")
                xdm_pid_match = re.search(r"(?im)^\s*(\d+)\s+.*\bxdm\b", xdm_ps)
                if xdm_pid_match:
                    serial_run(probe, serial, process,
                               f"echo TTY2-XDM-STOP >/dev/ttyS0; kill {xdm_pid_match.group(1)}",
                               "TTY2-XDM-STOP")
                else:
                    fail(f"XDM process missing after session end: {xdm_ps!r}")
                try:
                    switch_vt(probe, serial, process, 1)
                    wait_text(probe, process, "tty1-restored-login", "login:", timeout=60)
                except AssertionError as error:
                    fail(f"tty1 did not restore a text login after XDM exit: {error}")

            switch_vt(probe, serial, process, 2)
            serial_run(probe, serial, process,
                       "echo TTY2-BACK >/dev/ttyS0", "TTY2-BACK")
            xdm_log = collect(probe, serial, process, "XDM-LOG", path="/var/log/xdm.log")
            xorg_log = collect(probe, serial, process, "XORG-LOG", path="/var/log/Xorg.0.log")
            xdm_delta = fresh_log_suffix(xdm_before, xdm_log, "xdm.log")
            (OUTPUT / "xdm-evidence.log").write_text(xdm_delta + "\n")
            (OUTPUT / "Xorg.0.log").write_text(xorg_log + "\n")
            print("xorg-vm: evidence captured", flush=True)
    finally:
        if probe is not None:
            try:
                probe.frame("last-frame")
            finally:
                probe.close()
        if process.poll() is None:
            qmp_quit(qmp, process)
        qmp.unlink(missing_ok=True)

    serial_text = serial.read_text(errors="replace") if serial.exists() else ""
    handoff = [line for line in serial_text.splitlines() if "[reliefnt] VT " in line]
    (OUTPUT / "vt-handoff.log").write_text("\n".join(handoff) + "\n")
    try:
        require_vt_handshake(serial_text)
    except AssertionError as error:
        fail(f"kernel VT_PROCESS handshake evidence incomplete: {error}")
    if xdm_delta:
        try:
            require_event_order(xdm_delta)
        except AssertionError as error:
            fail(f"XDM lifecycle evidence invalid: {error}")
    elif session_ready:
        fail("no fresh XDM lifecycle evidence was captured")

    if failures:
        print(f"xorg-vm: FAIL ({len(failures)} problems)")
        return 1
    print("xorg-vm: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
