# Xorg + IceWM + XDM

ReliefOS boots the Xorg desktop session on tty1. Staging installs the Alpine
official Xorg, IceWM and XDM packages and writes the raw-root marker `xorg`;
the installer media rewrites the marker to run its native setup desktop.

The Xorg backend is intentionally small. tty1's existing `console-session`
runs `/usr/lib/reliefos/reliefos-xdm` in the foreground. XDM starts
`/usr/bin/Xorg :0 -config /etc/X11/xorg.conf vt1 -keeptty`; the fixed config
uses `/dev/fb0`, `/dev/input/event0` and `/dev/input/event1`. After PAM accepts
credentials, `/usr/lib/reliefos/xdm-session` runs as the authenticated user,
starts `xterm`, and keeps `icewm` in the foreground. Exiting IceWM returns
control to XDM; exiting XDM lets tty1's console session fall back to text
login. tty2 through tty6 remain text consoles.

`make fetch` downloads the official Alpine community `dillo` 3.3.0-r2 browser,
with its locked runtime dependencies from
the official Alpine v3.24 main and community repositories. Dillo uses FLTK's
X11 frontend and links directly to `libX11.so.6`. The framebuffer and GTK
NetSurf packages were
removed after failing to run in the VMware SVGA QEMU session; PCManFM was
dropped from the fetch list after it kept exiting silently without a window.

## Motif applications

The Xorg backend builds `calc`, `osver` and `fileman` as Motif X11 clients. Calculator
supports signed 64-bit integer expressions, parentheses, normal arithmetic
precedence, keyboard input and on-screen buttons. Division by zero, overflow
and malformed expressions display an error. System information shows the
ReliefOS logo and the ReliefNT kernel name, version, build time and copyright;
five left clicks on the logo within two seconds retain the kernel-debug
activation shortcut. Both windows support the IceWM close action.

File Manager provides an editable address bar, expandable folder list,
multiple selection, context menus, copy/cut/paste, new folders, rename,
shortcuts, direct deletion and tar creation/extraction through `/bin/tar`.
Deletion asks for confirmation, removes directory contents recursively and
removes symbolic links without following their targets. There is no Recycle
Bin or restore action. Tar runs with an argument array, without a shell, and
reports success only after the command exits successfully. A single directory
archive contains that directory's contents; multiple selections preserve each
selected entry's name. Extraction creates a folder named after the archive.
Its properties dialog displays file or directory size and edits mode, UID and
GID using the current user's permissions. Text files open in NEdit and HTML
files in Dillo; Open With accepts an installed X11 executable's absolute path.
The administrator password helper is not an X11 client, so protected
operations report permission errors rather than opening that helper.
Ctrl+L focuses the address bar, F5 refreshes,
Alt+Up goes to the parent, F2 renames, Ctrl+C/X/V copies/cuts/pastes, and both
Delete and Shift+Delete request direct deletion after confirmation.

The build extracts X11, Xt and Motif development headers and linker libraries
from checksum-pinned, signature-verified Alpine APKs into
`out/<profile>/upstream/x11-development`. These inputs use the same versions as
the guest runtime libraries. Development headers are build-only and are not
staged into the guest.

## IceWM theme, taskbar, menu and keys

The session ships one IceWM configuration, `system/xorg/icewm`, staged to
`/etc/reliefos/icewm`. `xdm-session` exports `ICEWM_PRIVCFG=/etc/reliefos/icewm`
before starting the window manager, and that directory is the first one IceWM
searches, so the shipped `theme`, `preferences`, `menu`, `toolbar`, `keys` and
`winoptions` are the only configuration in effect; no user or upstream default
can take over. The `theme` file selects `themes/light/default.theme`, the
"Light" theme, which keeps the previous palette on IceWM's `nice` look:
`#dce4f5` title bars with `#1a1b26` text, `#8aa0d6` active borders, white menus
with a `#3b62a6` selection, and an `#eceef4` taskbar and borders.

The taskbar runs along the bottom edge of a single workspace and shows the
window list and the clock. `xdm-session` starts xterm in the same colours as
before (`#f7f8fb` background, `#22242e` text, `#3b62a6` cursor), and the
`Terminal` menu and toolbar entries run `terminal.sh`, which starts that same
terminal.

The root menu opens with Super+Space and offers Terminal, File Manager,
Calculator, System information, Text editor (NEdit), Web browser (Dillo),
`xeyes`, `About` and `Log out`. The `About` entry opens the session identity in
a terminal window. `Log out` ends the session by terminating IceWM, which hands
control back to XDM.

| Keys | Action |
| --- | --- |
| Super+Return | new themed xterm |
| Super+Space | root menu |
| Super+Q | close window |
| Super+M | minimize window |
| Super+Z | maximize window |
| Super+Up / Down / Left / Right | tile window to top / bottom / left / right half |
| Alt+Tab / Alt+Shift+Tab | window switcher next / previous |

The root background is a retro pixel-art wallpaper
(`system/xorg/wallpaper.png`, 1280x800) that `xdm-session` applies with
`xwallpaper --focus` before IceWM starts; when xwallpaper is missing or fails
the X server keeps its default background.

Packages come from the pinned Alpine v3.24 x86_64/musl main and community
indexes. The lock file marks only the Xorg closure entries with `feature=xorg`;
that closure carries IceWM and its image and sound libraries (`imlib2`,
`librsvg`, `libao`, `libsndfile` and their providers) while `musl`,
`libasound.so.2`, `libmount.so.1`, `libuuid.so.1`, `libbsd.so.0` and
`libmd.so.0` are supplied by the local ReliefOS packages. `make fetch` is the
only network operation; ordinary builds use the verified cache and report the
missing dependency ID when an archive is absent. Native staging selects only
`feature=base` entries, while Xorg staging selects both features and preserves
the APK database, signatures and ownership manifests.

The XDM PAM service includes the existing `common-auth`, `common-account` and
`common-session` policy. The user session writes `$HOME/.xsession-errors` (or a
user cache fallback); it never writes root-only logs or records passwords and
authorization data. Installer runtime roots deliberately restore the native
marker, inittab and OpenRC links, while the installed target root keeps the
Xorg policy.

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
authentication, a non-root IceWM session, xterm, session exit and tty1 text
recovery in that order. This contract does not certify physical GPUs or
hardware-specific fbdev modes; those require a platform run with captured
evidence.
