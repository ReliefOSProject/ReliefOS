# Xorg + IceWM + XDM

ReliefOS boots the Xorg desktop session on tty1. Staging installs the Alpine
official Xorg, IceWM and XDM packages and writes the raw-root marker `xorg`;
the installer media rewrites the marker to run its native setup desktop.

The Xorg backend is intentionally small. tty1's existing `console-session`
runs `/usr/lib/reliefos/reliefos-xdm` in the foreground. XDM starts
`/usr/bin/Xorg :0 -config /etc/X11/xorg.conf vt1 -keeptty`. The shipped
`/etc/X11/xorg.conf` leaves the `Device` driver and device path unspecified so
stock Xorg auto-configuration can select `modesetting` for a supported DRM
device or `fbdev` when none is available. The fixed profile requests a software
cursor and disables KMS page flips; it retains the evdev keyboard and mouse on
`/dev/input/event0` and `/dev/input/event1`, including absolute-pointer
resynchronization. The server keeps its normal depth-24 screen configuration.
On a no-DRM fbdev boot, `/usr/lib/reliefos/xorg-tty-wrapper` runs the existing
BusyBox `fbset -fb /dev/fb0 -depth 32` immediately before Xorg when `/dev/fb0`
is a character device. This prepares the 32bpp framebuffer expected by the
24-bit true-color X screen; a failed `fbset` does not prevent Xorg from
starting. The wrapper passes the configured Xorg arguments unchanged and
skips the preflight when `/dev/dri/card0` is present. After PAM accepts
credentials, `/usr/lib/X11/xdm/Xsession` runs as the authenticated user,
starts `st` and the native `icewm-session` supervisor for IceWM and its
theme-managed background. Exiting IceWM returns
control to XDM; exiting XDM lets tty1's console session fall back to text
login. tty2 through tty6 remain text consoles.

Text-VT restoration replays the retained ANSI/UTF-8 history into a same-layout
RAM framebuffer, then copies every visible row and publishes the frame once.
The copy includes the strip below the text grid and excludes row padding. If
the temporary surface cannot be allocated or validated, direct replay preserves
the text and cursor semantics but can retain the earlier device-memory replay
latency.

XDM uses its compiled-in default `/etc/X11/xdm/xdm-config`, with no `-config`
argument from the launcher. Server commands and greeter resources live in
`/etc/X11/xdm/Xservers` and `/etc/X11/xdm/Xresources`; session and setup hooks
use `/usr/lib/X11/xdm/Xsession` and `/usr/lib/X11/xdm/Xsetup_0`.
The signed `reliefos-xdm-config` package owns these five policy files and
declares its dependency on and file replacement of the official `xdm` package.
It also depends on the local server, drivers and session programs used by the
profile. The upstream archive remains unchanged. Edit these standard paths to customize
XDM. The greeter uses the packaged gray background, blue greeting and X logo,
with the heading `Login into ReliefOS`. Its complete resource definitions live
in `Xresources`, without an include of that file into itself. The standard
`xdm-config` also lists `RELIEFOS_XORG_EVENT_FD` in
`DisplayManager.exportList` so the launcher's session-event descriptor reaches
Xsession. On the final e9cf guest, the actual root-owned XDM log recorded all
four expected lifecycle events in order with status 0. The host XDM fixture
is a model and does not replace this guest evidence.

## Post-acceptance upstream sync (2026-10-19)

The `e9cf25df756dfcb78eccae03d1e890a608019686d022086e88ead91c9b30743f`
kernel SHA and runtime results above remain historical evidence for that exact
image. The subsequent upstream merge incorporates console PTY semantics and
5.1.0 version/release updates while preserving the SVGA source. The combined
source has not been rebuilt into a new canonical image or rerun under native
VMware; **VMware 验证未完成**. The e9cf identity does not identify or validate
the merged source.

`make fetch` downloads the official Alpine community `dillo` 3.3.0-r2 browser,
with its locked runtime dependencies from
the official Alpine v3.24 main and community repositories. Dillo uses FLTK's
X11 frontend and links directly to `libX11.so.6`. The framebuffer and GTK
NetSurf packages were
removed after failing to run in the VMware SVGA QEMU session; PCManFM was
dropped from the fetch list after it kept exiting silently without a window.

## C++ / FLTK applications

The Xorg backend builds the ReliefOS application frontends with C++17 and
FLTK 1.3.11: `calc`, `osver`, `fileman`, `taskmgr`, `minesweeper`,
`leonmmcoset`, `xiaobai`, `paint`, `imageview`, `doomlauncher`, `installer`
and the ReliefOS `doom` platform frontend. Independent C models, file
operations, installation transactions, the TTY installer and the Doom engine
remain shared backend code, called through C linkage.

Calculator
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

Task Manager retains its process tree, process details and termination
confirmation, CPU/core/memory/GPU history and startup application management.
Paint supports BMP and PNG open/save, tools, brush sizes and colors, and
confirmation before discarding unsaved work. Image Viewer keeps sibling
navigation and fit/1x/2x zoom. The installer keeps its complete fresh/update
wizard, partition tools and confirmations, accounts and optional Xorg choice;
without `--graphical`, a terminal still uses the C TTY frontend without
opening an X display. Doom keeps its keyboard input, audio pacing, fullscreen
and headless test mode.

`userland/apps/fltk_support.h` initializes locale and the existing application
gettext domain using UTF-8. The shipped SimSun CJK font is selected for
widget labels, input text and dialogs through FLTK's Xft backend. Native
FLTK file chooser labels share the application translation catalog. Path
inputs reject excessive byte lengths instead of silently truncating a
multibyte filename. File/task browser lists disable FLTK format-character
interpretation for external names. Translation extraction includes C++
sources and the common UI headers.

The build extracts X11 and FLTK development headers and target linker libraries
from checksum-pinned, signature-verified Alpine APKs into
`out/<profile>/upstream/x11-development`. These inputs use the same versions as
the guest runtime libraries. Target `libstdc++` and `libgcc_s` are linked
explicitly; host glibc C++ libraries are not used. The frontends use the
FLTK/C APIs without STL, exceptions or RTTI. Development headers are
build-only and are not staged into the guest. The package dependency scanner
uses the actual ELF SONAME dependencies to select the runtime libraries for
the Xorg desktop and live graphical installer. Third-party NEdit still needs
its Motif runtime; it is not one of these migrated ReliefOS frontends.

## IceWM theme, taskbar, menu and keys

The session ships supplemental IceWM files from `system/xorg/icewm`, staged to
`/etc/reliefos/icewm` and exposed through `/etc/icewm`, IceWM's standard system
configuration path. Desktop and installer sessions leave `ICEWM_PRIVCFG`
unassigned, so personal configuration takes precedence over system defaults.
The ReliefOS menu keeps common application shortcuts and includes application
categories generated by the packaged `icewm-menu-fdo`. The ReliefOS directory
also supplies system preferences, keys, local scripts and theme overrides.
The system `theme` file selects `win95/default.theme` for users who have not
chosen a personal theme.

Both desktop and installer images include every theme and all associated
pixmaps from the pinned IceWM APK under `/usr/share/icewm/themes`. The bundled
themes are `CrystalBlue`, `Helix`, `Infadel2`, `NanoBlue`, `default`, `icedesert`,
`metal2`, `motif` and `win95`, including Infadel2's `Ergonomic` and `Overloaded`
variants. APK staging checks the entire upstream theme payload before
publishing an image; no theme selection needs an additional download.

Use IceWM's own **Settings -> Themes** submenu to switch themes. IceWM saves
the selection in the user's configuration directory, normally
`$HOME/.icewm/theme`, or `$HOME/.config/icewm/theme` when that directory already
exists. An existing `$XDG_CONFIG_HOME/icewm` directory or a user-provided
`ICEWM_PRIVCFG` is also supported. The selection is restored on subsequent
logins, and one user's choice does not change other users' themes.

The taskbar runs along the bottom edge of a single workspace and shows the
window list and the clock. `xdm-session` starts st in the same colours as
before (`#f7f8fb` background, `#22242e` text, `#3b62a6` cursor), and the
`Terminal` menu and toolbar entries run `terminal.sh`, which starts that same
terminal.

The root menu opens with Super+Space and offers Terminal, File Manager,
Calculator, System information, Text editor (NEdit), Web browser (Dillo),
`xeyes`, dynamic application categories, `About` and `Log out`. IceWM's packaged
`icewm-menu-fdo` scans `/usr/share/applications` and adds category submenus such
as Game, Graphics, Network, System and Utility; the migrated Calculator,
System information and File manager provide desktop entries there when their
components are staged. The `About` entry opens the session identity in
a terminal window. `Log out` ends the session by terminating IceWM, which hands
control back to XDM.

| Keys | Action |
| --- | --- |
| Super+Return | new themed st |
| Super+Space | root menu |
| Super+Q | close window |
| Super+M | minimize window |
| Super+Z | maximize window |
| Super+Up / Down / Left / Right | tile window to top / bottom / left / right half |
| Alt+Tab / Alt+Shift+Tab | window switcher next / previous |

XDM background setup belongs to `/usr/lib/X11/xdm/Xsetup_0`; the shipped hook
leaves the X server's background unchanged. Greeter colors, fonts and the logo
belong to `/etc/X11/xdm/Xresources`. No product wallpaper is imposed on either
the greeter or the desktop.

Both installed and installer desktops start `/usr/bin/icewm-session`, whose
native `icewmbg` process reads the selected theme's background settings and
follows theme changes. If a theme does not define a background, IceWM's own
defaults apply. The former `/etc/reliefos/wallpaper.png` is no longer staged.

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

The installed Alpine graphics stack uses the signed, hash-locked
`mesa-dri-gallium` 26.1.6-r0 APK with the existing Mesa 26.1.6, libdrm 2.4.134,
and Xorg 21.1.25 packages. Its `vmwgfx_dri.so` entry point supplies the
upstream VMware Gallium driver; the package is part of the `xorg` feature, so
console-only staging does not install it. No Mesa replacement library or
renderer-string override is used. See [SVGA3D.md](SVGA3D.md) for the supported
VGPU9/DRI3 boundaries and accepted final native and QEMU runtime evidence.

With this pinned, unmodified Mesa 26.1.6, the DRI renderer queries report
`accelerated=0` and `video_memory=1 MiB`. Upstream `svga_init_screen_caps()`
calls `u_init_pipe_screen_caps(..., 0)`, so Mesa sets `caps.accelerated` to zero
and returns that value from the DRI query; this bit alone does not identify
llvmpipe or the path that produced a frame. Mesa's video-memory query uses
`max_mob_memory_mib` and falls back to 1 MiB when it is zero; ReliefNT currently
returns zero for `DRM_VMW_PARAM_MAX_MOB_MEMORY` on this VGPU9 host-backed path.
Separately, the ReliefOS SVGA3D application path submits device commands, reads
back rendered pixels and presents frames for scanout (see [SVGA3D.md](SVGA3D.md));
that app-path evidence does not certify Xorg glamor or physical host-GPU use.
ReliefNT returns `EINVAL` for `DRM_VMW_PARAM_HW_CAPS` and `HW_CAPS2`; its
`GET_3D_CAP` table describes the supported VGPU9 interface without claiming
unimplemented GB/DX userspace interfaces.

The DRM primary and render nodes use Linux DAC permissions `0660` and group
`video` (GID 44). The base rootfs defines this group, the installer adds normal
users to it, and later standard user creation does the same. This grants device
access without making the user an administrator. See [ABI.md](ABI.md) for the
node names and permission contract.

The XDM PAM service includes the existing `common-auth`, `common-account` and
`common-session` policy. The user session writes `$HOME/.xsession-errors` (or a
user cache fallback); it never writes root-only logs or records passwords and
authorization data. Installer runtime roots deliberately restore the native
marker, inittab and OpenRC links, while the installed target root keeps the
Xorg policy.

Xorg consumes public Linux/POSIX interfaces only. ReliefNT exposes `/dev/tty0`
as the active VT and implements `VT_OPENQRY`, `VT_GETMODE/VT_SETMODE`,
`VT_RELDISP`, `KDGKBMODE`, `KDSKBMODE`, and the existing VT/KD calls. User-mode
#GP faults now follow the process-local SIGSEGV path; kernel-mode #GP remains
fatal. This containment does not identify or repair the upstream recursive-exit
caller observed in the earlier logout failure. An inherited `preadv2`
flag/error-order concern remains deferred, so these results do not certify
complete Linux/POSIX conformance. The Xorg ABI probe is
`python3 tools/test_xorg_abi.py --source-only`; kernel compilation and
subrepository ABI tests are separate checks.

For QEMU acceptance, capture serial/QMP evidence and run:

```sh
python3 tools/test_xorg_qemu.py --image out/xorg-test/images/reliefos.vmdk \
  --log out/xorg-test/qemu/xorg.log
```

The log must show `desktop-backend=xorg`, XDM on vt1, accepted PAM
authentication, a non-root IceWM session, st, session exit and tty1 text
recovery in that order. The unchanged repository lifecycle oracle passed on
both QEMU Standard VGA and VMware VGA 2D for the final source image. The fresh
tty1 numeric-prompt observations (`37192972` and `83680142`) were separate
OUT-only strict checks; they did not alter the repository oracle. The VMware
VGA 2D log retains the APIC 1 startup timeout followed by `online=2/2`; its
cause remains deferred. Native VMware with `mks.enable3d=FALSE` passed ordinary
session and text-restoration checks using llvmpipe/DRISWRAST software rendering;
Xorg did not use glamor for that fallback. See [SVGA3D.md](SVGA3D.md) for
accepted accelerated and software-fallback results and their exact scope. This
contract does not certify physical GPUs or hardware-specific fbdev modes.
