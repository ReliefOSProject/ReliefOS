# Xorg + TWM + XDM

ReliefOS has two mutually exclusive desktop backends in Kconfig. The default
is `CONFIG_DESKTOP_BACKEND_RELIEFOS=y`, which keeps the existing `windowd`,
`desktop.elf` and `sessiond` startup. Selecting
`CONFIG_DESKTOP_BACKEND_XORG=y` stages the Alpine official Xorg, TWM and XDM
packages and writes the raw-root marker `xorg`.

The Xorg backend is intentionally small. tty1's existing `console-session`
runs `/usr/lib/reliefos/reliefos-xdm` in the foreground. XDM starts
`/usr/bin/Xorg :0 -config /etc/X11/xorg.conf vt1 -keeptty`; the fixed config
uses `/dev/fb0`, `/dev/input/event0` and `/dev/input/event1`. After PAM accepts
credentials, `/usr/lib/reliefos/xdm-session` runs as the authenticated user,
starts `xterm`, and keeps `twm` in the foreground. Exiting TWM returns control
to XDM; exiting XDM lets tty1's console session fall back to text login. tty2
through tty6 remain text consoles.

`make fetch` downloads the official Alpine community `dillo` 3.3.0-r2 browser,
with its locked runtime dependencies from
the official Alpine v3.24 main and community repositories. Dillo uses FLTK's
X11 frontend and links directly to `libX11.so.6`. The framebuffer and GTK
NetSurf packages were
removed after failing to run in the VMware SVGA QEMU session; PCManFM was
dropped from the fetch list after it kept exiting silently without a window.

## Motif applications

The Xorg backend builds `calc` and `osver` as Motif X11 clients. Calculator
supports signed 64-bit integer expressions, parentheses, normal arithmetic
precedence, keyboard input and on-screen buttons. Division by zero, overflow
and malformed expressions display an error. System information shows the
ReliefOS logo and the ReliefNT kernel name, version, build time and copyright;
five left clicks on the logo within two seconds retain the kernel-debug
activation shortcut. Both windows support the TWM delete action. The native
desktop backend continues to build the original frontends from `native.c`.

The build extracts X11, Xt and Motif development headers and linker libraries
from checksum-pinned, signature-verified Alpine APKs into
`out/<profile>/upstream/x11-development`. These inputs use the same versions as
the guest runtime libraries. Development headers are build-only and are not
staged into the guest.

## TWM theme, menus and keys

The session ships one TWM configuration, `system/xorg/twmrc`, staged to
`/etc/reliefos/twmrc`. The "Light" theme uses only the guest misc bitmap fonts
(`fixed` 6x13 for menus and the icon manager list, `9x15` for title bars) with a
light blue palette: `#dce4f5` title bars with `#1a1b26` text, `#8aa0d6` borders,
white menus and a `#eceef4` desktop grey. `xdm-session` starts xterm in the same
colours (`#f7f8fb` background, `#22242e` text, `#3b62a6` cursor).

Window decoration keeps the stock TWM buttons: the default logo button on the
left and the resize button on the right of every title bar, with no custom title
buttons. The TWM icon manager is a vertical strip pinned to the right edge
(`IconManagerGeometry "192x800-0+0"`; TWM sizes its height to the entry count).
Its title bar is symmetric: "TWM Icon Manager" sits between equal 9px gaps
between the two default buttons.

The root menu, titled `ReliefOS`, opens from any root-button press or
Super+Space and offers an `Applications` submenu, window operations, `About`
and `Exit session`. The `Applications` submenu contains new-xterm, `xeyes`,
Dillo, NEdit, Calculator and System information. TWM menus
are hold-to-open: keep the button or key pressed, drag onto an entry and
release on it to run it. The `NEdit` entry opens the Motif text editor,
installed with its dependencies from the official Alpine APK repository by
`make fetch`. Note that this TWM build only delivers key bindings while the
pointer is over a window; over the bare root background, use the mouse to open
the menu.

| Keys | Action |
| --- | --- |
| Super+Return | new themed xterm |
| Super+Space | root menu (hold) |
| Super+Q | delete window |
| Super+M | iconify window |
| Super+A | toggle auto-raise for window |
| Alt+Tab / Alt+Shift+Tab | window ring next / previous |
| Super+Up / Down / Left / Right | zoom window to top / bottom / left / right half |
| Super+Z | zoom window to full screen |

The root background is a retro pixel-art wallpaper
(`system/xorg/wallpaper.png`, 1280x800) that `xdm-session` applies with
`xwallpaper --focus` before TWM starts; when xwallpaper is missing or fails the
X server keeps its default background.

Packages come from the pinned Alpine v3.24 x86_64/musl main and community
indexes. The lock file marks only the Xorg closure entries with `feature=xorg`.
`make fetch` is the only network operation; ordinary builds use the verified
cache and report the missing dependency ID when an archive is absent. Native
staging selects only `feature=base` entries, while Xorg staging selects both
features and preserves the APK database, signatures and ownership manifests.

The XDM PAM service includes the existing `common-auth`, `common-account` and
`common-session` policy. The user session writes `$HOME/.xsession-errors` (or a
user cache fallback); it never writes root-only logs or records passwords and
authorization data. Installer runtime roots deliberately restore the native
marker, inittab and OpenRC links, while the installed target root keeps the
user-selected Xorg policy.

Xorg consumes public Linux/POSIX interfaces only. ReliefNT exposes `/dev/tty0`
as the active VT and implements `VT_OPENQRY`, `VT_GETMODE/VT_SETMODE`,
`VT_RELDISP`, `KDGKBMODE`, `KDSKBMODE`, and the existing VT/KD calls. The
Xorg ABI probe is `python3 tools/test_xorg_abi.py --source-only`; kernel
compilation and subrepository ABI tests are separate checks.

For QEMU acceptance, capture serial/QMP evidence and run:

```sh
python3 tools/test_xorg_qemu.py --image out/xorg-test/images/reliefos.vmdk \
  --log out/xorg-test/qemu/xorg.log
```

The log must show `desktop-backend=xorg`, XDM on vt1, accepted PAM
authentication, a non-root TWM session, xterm, session exit and tty1 text
recovery in that order. This contract does not certify physical GPUs or
hardware-specific fbdev modes; those require a platform run with captured
evidence.
