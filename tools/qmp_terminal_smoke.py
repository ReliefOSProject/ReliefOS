#!/usr/bin/env python3
import json
from pathlib import Path
import socket
import sys
import time


def recv_some(sock: socket.socket) -> None:
    sock.settimeout(0.2)
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data or len(data) < 4096:
            break


def send(sock: socket.socket, obj: dict, delay: float = 0.1) -> None:
    sock.sendall((json.dumps(obj) + "\r\n").encode())
    time.sleep(delay)
    recv_some(sock)


def hmp(sock: socket.socket, cmdline: str, delay: float = 0.1) -> None:
    send(sock, {"execute": "human-monitor-command", "arguments": {"command-line": cmdline}}, delay)


def send_keys(sock: socket.socket, keys: tuple[str, ...], delay: float = 0.08) -> None:
    for key in keys:
        hmp(sock, f"sendkey {key}", delay)


def text_keys(text: str) -> tuple[str, ...]:
    special = {
        " ": "spc",
        ".": "dot",
        "/": "slash",
        "\\": "backslash",
        "=": "equal",
        "-": "minus",
        "_": "shift-minus",
        ":": "shift-semicolon",
        # QEMU's HMP key name for the pipe key is the shifted backslash key.
        # `bar` is accepted by some builds but does not inject a character on
        # the guest keyboard layout used by ReliefOS.
        "|": "shift-backslash",
        "&": "shift-7",
    }
    keys: list[str] = []
    for character in text:
        if "A" <= character <= "Z":
            keys.append(f"shift-{character.lower()}")
        else:
            keys.append(special.get(character, character))
    return tuple(keys)


def main() -> int:
    arguments = sys.argv[1:]
    skip_oobe = False
    exit_only = False
    gcc_smoke = False
    desktop_app: str | None = None
    login_password: str | None = None
    editor = "vim"
    fastfetch_smoke = False
    fastfetch_single = False
    hyfetch_smoke = False
    sl_smoke = False
    serial_log_path: Path | None = None
    start_menu_smoke = False
    iso9660_smoke = False
    dynlinkerror_smoke = False
    fancy_prompt_smoke = False
    abittest_smoke = False
    if arguments and arguments[0] == "--skip-oobe":
        skip_oobe = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--exit-only":
        exit_only = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--gcc":
        gcc_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--fastfetch":
        fastfetch_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--fastfetch-single":
        fastfetch_single = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--hyfetch":
        hyfetch_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--sl":
        sl_smoke = True
        arguments = arguments[1:]
    if len(arguments) >= 2 and arguments[0] == "--serial-log":
        serial_log_path = Path(arguments[1])
        arguments = arguments[2:]
    if arguments and arguments[0] == "--start-menu":
        start_menu_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--iso9660":
        iso9660_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--dynlinkerror":
        dynlinkerror_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--fancy-prompt":
        fancy_prompt_smoke = True
        arguments = arguments[1:]
    if arguments and arguments[0] == "--abittest":
        abittest_smoke = True
        arguments = arguments[1:]
    if len(arguments) >= 2 and arguments[0] == "--desktop-app":
        desktop_app = arguments[1]
        arguments = arguments[2:]
    if len(arguments) >= 2 and arguments[0] == "--login-password":
        login_password = arguments[1]
        arguments = arguments[2:]
    if len(arguments) >= 2 and arguments[0] == "--editor":
        editor = arguments[1]
        arguments = arguments[2:]
    if editor not in ("nano", "pleditor", "vi", "vim"):
        return 2
    if desktop_app is not None and (not desktop_app.isascii() or not desktop_app.isalnum()):
        return 2
    if desktop_app is not None and (fastfetch_smoke or fastfetch_single or hyfetch_smoke or sl_smoke or start_menu_smoke or iso9660_smoke or
                                    dynlinkerror_smoke or fancy_prompt_smoke or abittest_smoke or exit_only):
        return 2
    if len(arguments) != 1 or (login_password is not None and not skip_oobe):
        return 2
    sock_path = arguments[0]
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(200):
        try:
            sock.connect(sock_path)
            break
        except OSError:
            time.sleep(0.05)
    else:
        return 3

    recv_some(sock)
    send(sock, {"execute": "qmp_capabilities"})
    # The full UEFI boot and userland handoff take about 30 seconds under
    # headless QEMU.  Send the OOBE keys only after its fullscreen window is
    # accepting GUI events.
    time.sleep(30.0)

    if not skip_oobe:
        # A freshly generated image opens OOBE.  The default username is admin
        # and the password field owns focus, so create the first account.
        send_keys(sock, ("n", "a", "n", "o", "t", "e", "s", "t", "ret"))
        # First-run OOBE now completes by returning to the normal Sign in
        # screen.  Its selected account is admin and the password field owns
        # focus, so authenticate before the Start-menu input is sent.
        time.sleep(8.0)
        send_keys(sock, ("n", "a", "n", "o", "t", "e", "s", "t", "ret"))
        time.sleep(5.0)
    elif login_password is not None:
        hmp(sock, "screendump build/images/login-qmp-smoke.ppm", 0.4)
        send_keys(sock, tuple(login_password) + ("ret",))
        time.sleep(2.0)
        hmp(sock, "screendump build/images/desktop-qmp-smoke.ppm", 0.4)
    hmp(sock, "sendkey meta_l", 0.5)
    # Opening Start is asynchronous. Give the menu time to claim keyboard
    # focus before the search text starts arriving.
    time.sleep(1.5)

    if start_menu_smoke:
        hmp(sock, "screendump build/images/start-menu-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if desktop_app is not None:
        # GUI examples are normal desktop apps, so start them through the same
        # Start-menu search route a user uses.  The screenshot is the runtime
        # assertion: it catches failures after process spawn, window creation
        # or the first framebuffer presentation.
        send_keys(sock, text_keys(desktop_app) + ("ret",))
        time.sleep(10.0)
        hmp(sock, f"screendump build/images/{desktop_app}-qmp-smoke.ppm", 0.4)
        # glxgears handles Escape through its PortableGL event adapter.  Use
        # that explicit path so the smoke test verifies application cleanup,
        # while other desktop apps retain the window-server Alt-F4 path.
        hmp(sock, "sendkey esc" if desktop_app == "glxgears" else "sendkey alt-f4", 2.0)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    send_keys(sock, ("t", "e", "r", "m", "i", "n", "a", "l", "ret"))
    # Wait for Terminal to create its window and for BusyBox to enter the PTY
    # read loop before sending the editor command.  A cold guest may still be
    # loading the font and starting BusyBox after the window first appears.
    time.sleep(18.0)

    if abittest_smoke:
        send_keys(sock, text_keys("abittest") + ("ret",))
        time.sleep(8.0)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if gcc_smoke:
        for command, delay in (
            ("musl-gcc --version", 2),
            ("musl-gcc -static /usr/share/examples/musl-gcc/hello.c -o /tmp/gcc-hello", 30),
            ("/tmp/gcc-hello", 3),
            ("ld --version", 2),
        ):
            send_keys(sock, text_keys(command) + ("ret",))
            time.sleep(delay)
        hmp(sock, "screendump build/images/gcc-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if fastfetch_single:
        send_keys(sock, text_keys("fastfetch") + ("ret",))
        time.sleep(3.0)
        hmp(sock, "screendump build/images/fastfetch-single-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if hyfetch_smoke:
        send_keys(sock, text_keys("sh /usr/lib/reliefos/tests/hyfetch.sh") + ("ret",))
        time.sleep(6.0)
        # Accept the upstream wizard's defaults. Stop as soon as the wrapper
        # reports its repeated run, so keystrokes cannot run shell commands.
        for _ in range(14):
            if serial_log_path and "[hyfetch] repeat-status=" in serial_log_path.read_text(errors="replace"):
                break
            send_keys(sock, ("ret",))
            time.sleep(3.0)
        hmp(sock, "screendump build/images/hyfetch-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if sl_smoke:
        send_keys(sock, text_keys("sl -l") + ("ret",))
        # The locomotive remains visible during its run. Capture an active
        # frame, then wait for the upstream animation to leave the screen.
        time.sleep(2.0)
        hmp(sock, "screendump build/images/sl-qmp-smoke.ppm", 0.4)
        time.sleep(14.0)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    # Exercise terminal tab lifecycle: create a second PTY, close it, and
    # create it again so a closed tab cannot exhaust the kernel PTY pool.
    hmp(sock, "sendkey ctrl-shift-t", 1.0)
    hmp(sock, "sendkey ctrl-shift-w", 1.0)
    hmp(sock, "sendkey ctrl-shift-t", 1.0)
    time.sleep(2.0)

    if fancy_prompt_smoke:
        # This deliberately uses an unquoted shell assignment so the injected
        # backslash remains in PS1 for Ash's Fancy Prompt parser. `pwd` keeps
        # the expected current directory in the captured frame as well.
        send_keys(sock, text_keys("PS1=\\\\w") + ("ret",))
        time.sleep(1.0)
        send_keys(sock, text_keys("pwd") + ("ret",))
        time.sleep(2.0)
        hmp(sock, "screendump build/images/fancy-prompt-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    # Exercise the MMU Ash path before launching the editor: both pipeline
    # stages must be real fork/exec children, and the background job must be
    # visible to jobs and collectable by wait.
    for command in ("printf hello | wc -c", "sleep 2 &", "jobs", "wait"):
        send_keys(sock, text_keys(command) + ("ret",))
        time.sleep(2.0)

    if fastfetch_smoke:
        # Exercise the port's default summary, text-only mode, built-in ASCII
        # logo lookup, layout settings and restricted structure dispatcher.
        commands = (
            "fastfetch",
            "fastfetch --pipe",
            "fastfetch --logo arch --logo-padding-right 2",
            "fastfetch --logo windows --logo-position top --structure OS:DateTime:Version",
            "fastfetch --structure Title:Break:Version --structure-disabled Break",
        )
        for command in commands:
            send_keys(sock, text_keys(command) + ("ret",))
            time.sleep(2.0)
        hmp(sock, "screendump build/images/fastfetch-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if iso9660_smoke:
        # A data-only ISO attached as AHCI ATAPI must be mounted as /media/cdrom0.
        # Run each command separately so a failed cd remains visible in the
        # captured terminal instead of being hidden by shell command chaining.
        for command in ("cd /media/cdrom0", "pwd", "ls", "cat readme.txt"):
            send_keys(sock, text_keys(command) + ("ret",))
            time.sleep(2.0)
        hmp(sock, "screendump build/images/iso9660-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if dynlinkerror_smoke:
        # Terminal and desktop have the runtime resident already.  Deleting
        # the on-disk runtime must therefore leave the desktop available to
        # display the statically linked recovery window for the next launch.
        send_keys(sock, text_keys("rm /usr/lib/reliefos/libreliefos.so.2") + ("ret",))
        time.sleep(2.0)
        send_keys(sock, text_keys("pleditor") + ("ret",))
        time.sleep(5.0)
        hmp(sock, "screendump build/images/dynlinkerror-qmp-smoke.ppm", 0.4)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    # Both terminal editors are launched by BusyBox as external children
    # through the PTY.  They must write a file, restore the terminal and hand
    # control back to the resident shell.
    filename = {
        "nano": "nanotest.txt",
        "pleditor": "pleditortest.txt",
        "vi": "vitest.txt",
        # Keep the file in the login user's writable working directory.  The
        # image does not promise a pre-created /tmp hierarchy, and Vim must
        # exercise its normal write/quit path rather than fail with E212.
        "vim": "vimtest.txt",
    }[editor]
    editor_command = (f"vim -n {filename}"
                      if editor == "vim" else f"{editor} {filename}")
    send_keys(sock, text_keys(editor_command) + ("ret",))
    # The editor is loaded lazily; wait until its first userspace scheduling
    # turn before sending raw-mode input.
    time.sleep(5.0)
    if editor == "vim" and serial_log_path is not None:
        serial_text = serial_log_path.read_text(encoding="utf-8", errors="replace")
        if not any(path in serial_text for path in
                   ("path=/bin/vim ", "path=/usr/bin/vim ")):
            hmp(sock, "screendump build/images/vim-launch-failed.ppm", 0.4)
            raise RuntimeError("Vim was not executed by the Terminal shell")
    if exit_only:
        hmp(sock, "sendkey ctrl-x" if editor == "nano" else
            ("sendkey ctrl-q" if editor == "pleditor" else "sendkey esc"), 20.0)
        send(sock, {"execute": "quit"}, 0.2)
        return 0

    if editor == "nano":
        send_keys(sock, ("n", "a", "n", "o", "s", "m", "o", "k", "e"))
        hmp(sock, "sendkey ctrl-o", 0.3)
        hmp(sock, "sendkey ret", 2.0)
        hmp(sock, "sendkey ctrl-x", 2.0)
    elif editor == "pleditor":
        send_keys(sock, ("p", "l", "o", "s", "s", "m", "o", "k", "e"))
        hmp(sock, "sendkey ctrl-s", 2.0)
        hmp(sock, "sendkey ctrl-q", 2.0)
    else:
        send_keys(sock, ("i",) + text_keys("vismoke") + ("esc",))
        if editor == "vim":
            hmp(sock, "screendump build/images/vim-active-qmp-smoke.ppm", 0.4)
        send_keys(sock, text_keys(":wq") + ("ret",))

    send_keys(sock, text_keys(f"cat {filename}") + ("ret",))

    time.sleep(2.0)
    hmp(sock, f"screendump build/images/{editor}-qmp-smoke.ppm", 0.4)
    send(sock, {"execute": "quit"}, 0.2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
