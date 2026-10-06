# Third-Party Code

ReliefOS keeps third-party source code in `third_party/` and records Git-backed
dependencies as submodules.

## External Native Toolchain Test Fixture

The optional regression fixture packages the unchanged x86-64 archive from
[Dyne musl 2.2.0](https://github.com/dyne/musl/releases/tag/2.2.0): GCC 15.1.0,
binutils 2.44, musl headers/static libraries, libgcc and libstdc++, plus the
upstream supplementary sysroot. SHA256:
`31420e4f978e7ccbcc597ca5e18c2dcbe640ea6985ed6325112d8a171a72ece3`.
GCC/binutils use GPL-3.0-or-later; applicable GCC runtime libraries include the
GCC Runtime Library Exception. Musl uses MIT. Bundled library notices remain
inside `/opt/dyne`; compiler license texts and file hashes are installed under
`/usr/share/licenses/musl-gcc`. See `userland/musl-gcc/README.md` for source/build
recipe references, cache configuration and unchanged-binary verification.
It is no longer a production component: normal ISO, installer, VMDK and SDK
builds do not build or include it. GCC will be supplied by apk after integration.

## External Python Test Fixture

The optional regression fixture uses the user-provided static musl CPython
3.14.7 archive `cpython-3.14.7+20260901-x86_64-unknown-linux-musl-lto+static-full.tar.zst`.
SHA256: `e5a76e5893c39c89ed268ad71c3d6794c3be6b7236ec613449140c57686fc17f`.
The unmodified executable and full installation are in `/opt/python`.
The archive's CPython and bundled dependency license texts, `PYTHON.json`, and
package hashes are installed at `/usr/share/licenses/python`. CPython's license
is recorded in `LICENSE.cpython.txt`; other bundled libraries retain their
own notices. See `userland/python/README.md` for reproducible packaging and
the static command launcher. Python is no longer included in production
images or installer options; it will be supplied by apk after integration.

## Git Submodule Inventory

The following inventory covers every Git submodule declared by the root
`.gitmodules` file, plus the nested submodule declared by StardustUI. The
commits are the revisions recorded by the ReliefOS checkout.

| Path | Upstream | Pinned commit |
| --- | --- | --- |
| `third_party/busybox` | `https://github.com/mirror/busybox.git` | `1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4` |
| `third_party/libpng` | `https://github.com/pnggroup/libpng.git` | `3061454d980de7d53608f594194cfac722721d2a` |
| `third_party/litehtml` | `https://github.com/litehtml/litehtml.git` | `b9e89f0b9494ff9a5f008800af35503efabddf59` |
| `third_party/mbedtls` | `https://github.com/Mbed-TLS/mbedtls.git` | `068ff080b369adfac81509f9b57b2afabaf82dc5` |
| `third_party/ncurses` | `https://github.com/ThomasDickey/ncurses-snapshots.git` | `0096bd402c4a9c8f39bd7ed266e1b8920327e4d8` |
| `third_party/musl` | `https://git.musl-libc.org/git/musl` | `9fa28ece75d8a2191de7c5bb53bed224c5947417` |
| `third_party/mimalloc` | `https://github.com/microsoft/mimalloc` | `34fbd7e7cd4627424490afe19b20f8066bfc537d` |
| `third_party/pl_editor` | `https://github.com/Leonmmcoset/pl_editor.git` | `22fae7a1bc2362486d8bf845f0daf6ec7060a3a1` |
| `third_party/sl` | `https://github.com/mtoyoda/sl.git` | `923e7d7ebc5c1f009755bdeb789ac25658ccce03` |
| `third_party/sqlite` | `https://github.com/sqlite/sqlite.git` | `f3d536d37825302e31ed0eddd811c689f38f85a3` |
| `third_party/stardustui` | `https://github.com/xingji-studio/StardustUI.git` | `67aae17214a0d27bb6a8b0caf10b7c1f98313086` |
| `third_party/stardustui/third_party/ab_glyph_rasterizer/upstream` | `https://github.com/alexheretic/ab-glyph.git` | `791b15214d376dec06ae1c886da4c5f92f31e2e0` |
| `third_party/zlib` | `https://github.com/madler/zlib.git` | `da607da739fa6047df13e66a2af6b8bec7c2a498` |

## Mbed TLS

- Path: `third_party/mbedtls`
- Upstream: `https://github.com/Mbed-TLS/mbedtls.git`
- Version: `3.6.7`
- Git submodule commit: `068ff080b369adfac81509f9b57b2afabaf82dc5` (`v3.6.7`)
- License: Apache-2.0 (selected from the upstream dual Apache-2.0 or
  GPL-2.0-or-later terms; see `third_party/mbedtls/LICENSE`).

ReliefOS builds a TLS 1.2 client profile with certificate and hostname
verification for the shared HTTP client. The system image includes
`/etc/ssl/certs/ca-certificates.crt`, the curl CA Extract from
`https://curl.se/ca/cacert.pem`, to establish public Web PKI trust.

## musl and mimalloc

The default userland uses pinned musl 1.2.6 for Linux x86-64 C/POSIX,
TLS, pthreads, startup and dynamic linking. mimalloc 3.5.1 provides application
allocation. License texts are in `third_party/musl/COPYRIGHT` and
`third_party/mimalloc/LICENSE` and ship in images and SDKs.
The recorded `patches/musl/0001-enforce-password-file-lock.patch` replaces musl's
no-op password-file lock for PAM/account writers. It is applied to an isolated
build source, leaves the pinned submodule unchanged and is listed with its hash
in the runtime's `.leonos-musl.json` metadata.
`libreliefos.so.2` and `libreliefos.a` provide ReliefOS extensions; they contain no
replacement standard POSIX implementation. The compatible `libleonos.so.2` and
`libleonos.a` remain available for existing ABI-v2 applications. Pre-musl binaries
covered by the earlier migration must still be rebuilt.
See `MUSL_MIGRATION_2026-09-08.md` for exact validation and remaining gaps.

## Alpine package signing keys

The apk preparation seed includes the three x86_64 public keys selected by
Alpine's `alpine-keys` 2.6-r0 APKBUILD for v3.24. They are copied unchanged to
`/etc/apk/keys`; the official SHA-512 values, source URLs and MIT license
declaration are recorded under `/usr/share/licenses/alpine-keys`. This trust
store is separate from the HTTPS CA bundle and includes no private keys.
See [APK_PREPARATION.md](APK_PREPARATION.md) for the repository/database scope.

## Authentication Dependencies

Pinned official sudo 1.9.17p2, Linux-PAM 1.7.2, util-linux 2.41.6,
libxcrypt 4.5.2, shadow-utils 4.20.2, libbsd 0.12.2 and libmd 1.2.0 are
downloaded through the configured proxy and verified against
`configs/auth-upstream.json`. The manifest records each official release URL,
SHA256 and checksum source. They are built with the pinned project musl into
`build/auth-upstream/root`, independently of host accounts. The build installs
full upstream license notices under `usr/share/licenses/<package>` and records
commands and platform patches in `build/auth-upstream/*-build.json`.

shadow-utils supplies the upstream PAM passwd and local account tools; libbsd
supplies its readpassphrase dependency and requires libmd. Adding these build
dependencies does not activate standard accounts in the normal image. Exact
licenses, component states, disabled optional integrations and runtime evidence
are listed in [SUDOERS_PAM_UPSTREAM.md](SUDOERS_PAM_UPSTREAM.md) and
[SUDOERS_PAM_STATUS.md](SUDOERS_PAM_STATUS.md).

## StardustUI


- Path: `third_party/stardustui`
- Upstream: `https://github.com/xingji-studio/StardustUI.git`
- Pinned commit: `67aae17214a0d27bb6a8b0caf10b7c1f98313086`
- License: MIT; preserve `third_party/stardustui/LICENSE`. The generated SDK
  includes the library's public headers, ReliefOS C++ compatibility headers and
  the full license text.

ReliefOS builds StardustUI as `libstardustui.a` over its existing pixel-buffer
window ABI and UI text renderer. The image includes the upstream Hello World,
layout and widget-showcase examples at `/usr/lib/reliefos/apps/stardusthello/`,
`/usr/lib/reliefos/apps/stardustlayout/` and `/usr/lib/reliefos/apps/stardustshowcase/`, plus the
upstream Material 3 example themes in `/etc/stardustui/theme/`. StardustUI's
socket API is linked but currently reports that networking is unavailable, so
the network-dependent DuckChat example is intentionally not installed.

StardustUI also records one nested dependency:

- Path: `third_party/stardustui/third_party/ab_glyph_rasterizer/upstream`
- Upstream: `https://github.com/alexheretic/ab-glyph.git`
- Pinned commit: `791b15214d376dec06ae1c886da4c5f92f31e2e0`
- Crate: `ab_glyph_rasterizer` 0.1.10
- License: Apache-2.0; preserve the upstream license and attribution files.

## zlib

- Path: `third_party/zlib`
- Upstream: `https://github.com/madler/zlib.git`
- Version: `1.3.2`
- Pinned commit: `da607da739fa6047df13e66a2af6b8bec7c2a498` (`v1.3.2`)
- License: zlib License; preserve `third_party/zlib/LICENSE`.

ReliefOS builds zlib's freestanding in-memory compression/decompression core as
`libz.a`.  Gzip file-stream helpers are intentionally excluded because ReliefOS
does not provide a hosted stdio file backend to the library.

## libpng

- Path: `third_party/libpng`
- Upstream: `https://github.com/pnggroup/libpng.git`
- Version: `1.6.58`
- Pinned commit: `3061454d980de7d53608f594194cfac722721d2a` (`v1.6.58`)
- License: libpng License; preserve `third_party/libpng/LICENSE`.

ReliefOS builds libpng as `libpng.a` against its bundled zlib.  The public SDK
includes both upstream libraries and headers, while `reliefos/png.h` provides a
bounded PNG-to-ReliefOS-pixel decoder for ordinary GUI applications.

## SQLite

- Path: `third_party/sqlite`
- Upstream: `https://github.com/sqlite/sqlite.git`
- Version: 3.46.1; pinned commit:
  `f3d536d37825302e31ed0eddd811c689f38f85a3`
- License: SQLite public domain dedication and blessing; preserve
  `third_party/sqlite/LICENSE.md`.

ReliefOS installs the ABI-v1 shared library as `/usr/lib/sqlite.so.3` and
packages `sqlite3.h` in the SDK. The port uses a ReliefOS VFS and currently
disables WAL, loadable extensions, and cross-process file locking.

## findutils

- Official Alpine v3.24 `findutils` APK, version `4.10.0-r1`.
- SHA256: `86cf2b3f8aa3b1092cb3225fcc8179fc1229d178aa126266cfeadfda09487dc2`.
- License: GPL-3.0-or-later.
- Provides `/usr/bin/find` and `/usr/bin/xargs`; these commands are disabled
  in the BusyBox profile to prevent path ownership conflicts.

## BusyBox

- Path: `third_party/busybox`
- Upstream: `https://github.com/mirror/busybox.git`
- Version: `1.36.1`
- Pinned commit: `1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4` (`1_36_1`)
- License: GPL-2.0-only; the complete upstream `LICENSE` is staged at
  `/usr/lib/reliefos/apps/busybox/LICENSE` beside the executable.

ReliefOS builds a static, basic-applet BusyBox profile at
`/bin/busybox`. It includes file/text utilities such as
`ls`, `pwd`, `cat`, `echo`, `head`, `tail`, `wc`, `diff`, `less`, `mkdir`,
`rmdir`, `cp`, `mv`, `rm`, `unlink`, `printenv`, `uname`, `sleep`, `true`,
`false`, `nohup`, `vi`, and `printf`. The `sh` entry point is BusyBox Ash built for
ReliefOS's MMU path. It uses the kernel COW `fork`/`execve` ABI, inherited file
descriptors, process groups and PTY foreground groups for pipelines,
redirection, background jobs, and `jobs`/`fg`/`bg`. The image profile selects
Ash as its shell implementation. `nohup` is available from both the GUI
Terminal and TTY shell; it ignores `SIGHUP`, uses `/dev/null` for terminal
stdin, and appends terminal output to `nohup.out` with the usual `$HOME`
fallback.

The production recipe exports committed upstream BusyBox sources and verifies
them before and after building. It does not inject private applets, headers or
`libleonos.a`. The existing musl/mimalloc runtime remains. `sync` is the upstream
BusyBox applet. Storage commands are packaged separately: util-linux 2.41.6
supplies fdisk/sfdisk, mount/umount, blkid, lsblk and fsck; e2fsprogs 1.47.3,
dosfstools 4.2 and exfatprogs 1.4.3 supply filesystem tools. Fixed archives and
checksums are in `configs/auth-upstream.json` and `configs/storage-upstream.json`;
notices are installed in `/usr/share/licenses/<package>/`.
`leonos-grub-installer` is a ReliefOS shell helper copying an existing EFI payload,
not upstream grub-install. See `docs/UPSTREAM_TOOLS.md` for host, image and guest
evidence and remaining kernel compatibility gaps. Availability in an image does
not certify every operation or filesystem feature.

## ncurses and Vim

ncurses 6.6 is a default component, built from the pinned, unmodified upstream
submodule by `make upstream-ncurses`. Vim is no longer compiled by ReliefOS: the
signed upstream Alpine `vim`, `vim-common` and `xxd` packages are preinstalled
instead and carry their own package license metadata.

Normal images, the live installer and its installed payload contain
`/usr/bin/vim`, `/usr/share/vim/vim92`, ncurses utilities in `/usr/bin`,
and `/usr/share/terminfo`. The ncurses utilities are static Linux
executables. The developer and musl SDKs include upstream curses headers,
`libncursesw.a`, `libtinfow.a`, panel/menu/form archives and terminfo data.
Link wide-character applications with `-lncursesw -ltinfow`.

The earlier internal ANSI curses implementation remains an implementation
detail of existing ReliefOS applications; its headers are not the SDK's ncurses
interface. Licenses ship as `/usr/share/licenses/ncurses/COPYING`, with
`THIRD_PARTY/NCURSES-COPYING` in the developer SDK.
`python3 tools/test_terminal_packages.py` runs the ncurses binaries and library on
Linux; guest validation is documented separately.

## TinyCC and Lua

TinyCC and Lua are no longer bundled or compiled from source; their source
submodules and development components were deleted. Users who want them
install the official repository packages with `apk add tcc` and
`apk add lua5.4`. file/libmagic, GNU less and Vim are likewise official
repository packages (`file`, `libmagic`, `less`, `libncursesw`,
`ncurses-terminfo-base`, `vim`, `vim-common`, `xxd`); those are preinstalled
from the locked `alpine-*` archives by `make fetch` and the APK transaction
instead of being compiled.

## PL Editor

- Path: `third_party/pl_editor`
- Upstream modified fork: `https://github.com/Leonmmcoset/pl_editor.git`
- Pinned commit: `22fae7a1bc2362486d8bf845f0daf6ec7060a3a1`
- License: MIT; the complete upstream `LICENSE` is staged at
  `/usr/lib/reliefos/apps/pleditor/LICENSE` beside the executable.

ReliefOS builds PL Editor at `/usr/lib/reliefos/apps/pleditor/pleditor.elf`. Its upstream
platform-independent editor core is kept as a submodule; the ReliefOS platform
adapter provides raw PTY input, ANSI terminal output, terminal sizing and
multi-encoding file persistence. It is launched through Terminal and supports
syntax highlighting, search, undo/redo, line numbers, automatic bracket
completion, CRLF preservation, wrapped welcome messages and the fork's
extended syntax set.

## file / libmagic

file and libmagic are no longer compiled by ReliefOS. The signed upstream Alpine
`file` and `libmagic` packages are preinstalled instead: `/usr/bin/file` comes
from `file`, and `libmagic.so.1` plus the compiled magic database
`/usr/share/misc/magic.mgc` come from `libmagic`. Their license metadata
travels with the packages.

## Fastfetch

- Upstream: `https://github.com/fastfetch-cli/fastfetch.git`
- Packaged fork: `https://github.com/VasilyZa/fastfetch`
- Release: `https://github.com/VasilyZa/fastfetch/releases/download/2.68.1/fastfetch`
- Packaged version: `2.68.1`, supplied native x86-64 static musl build with
  the legacy LeonOS logo alias. Binary SHA-256:
  `25107efd56d0286059487bab17d964a6ec72275263de2ab09095a46637b06be1`.
- The old Fastfetch submodule and ReliefOS adapter have been removed.
- License: MIT; `userland/fastfetch/LICENSE` preserves the complete upstream
  license, matching the supplied build's source license byte for byte, staged at
  `/usr/share/licenses/fastfetch/LICENSE`.

`tools/package_fastfetch.py` downloads, validates and copies the release unchanged
to `/usr/lib/reliefos/apps/fastfetch/fastfetch.elf`, reached through
`/usr/bin/fastfetch`. It reads Linux interfaces directly; no ReliefOS detection
adapter is linked. `/etc/fastfetch/config.jsonc` selects the built-in logo through the legacy
`--logo LeonOS` alias and the normal configuration mechanism. See
`userland/fastfetch/README.md` for the proxy, cache, offline input, and checks.

## sl

- Path: `third_party/sl`
- Upstream: `https://github.com/mtoyoda/sl.git`
- Pinned commit: `923e7d7ebc5c1f009755bdeb789ac25658ccce03`
- License: permissive upstream license; the complete upstream `LICENSE` is
  staged at `/usr/lib/reliefos/apps/sl/LICENSE` beside the executable.

ReliefOS builds the Steam Locomotive joke command at
`/usr/lib/reliefos/apps/sl/sl.elf`. The upstream animation is kept intact and its curses
calls are implemented by the ANSI adapter in `userland/sl`.

## minimp3

- Path: `third_party/minimp3/minimp3.h`
- Upstream: `https://github.com/lieff/minimp3`
- Pinned commit: `ea99364f61c14656440e8d77e9c233ccf3124633`
- License: CC0-1.0; upstream declares the source public domain dedication and
  warranty disclaimer in the vendored header.

## GNU GRUB

- Delivered component: UEFI and Multiboot2 boot support in normal and installer
  media.
- Upstream: `https://www.gnu.org/software/grub/`
- License: GNU General Public License version 3 or later (GPL-3.0-or-later).

## litehtml

- Path: `third_party/litehtml`
- Upstream: `https://github.com/litehtml/litehtml.git`
- Recorded commit: `b9e89f0b9494ff9a5f008800af35503efabddf59`
- License: New BSD License / BSD-3-Clause, see `third_party/litehtml/LICENSE`

`browser.elf` does not yet link upstream litehtml directly because ReliefOS
userland is still freestanding C without a C++ runtime or STL. The browser now
uses `userland/apps/browser/litehtml_core.c` as the staged C document layout
core. That keeps the browser shell, network loading, history, and GUI wiring
ready for a later full litehtml container.

## Gumbo HTML Parser

- Path: `third_party/litehtml/src/gumbo`
- Upstream: `https://github.com/google/gumbo-parser`
- License: Apache License 2.0, see `third_party/litehtml/src/gumbo/LICENSE`.

Gumbo source is present through the litehtml submodule. It is not built or
executed by the current ReliefOS browser path.

## Microsoft Fonts

- Resources: `system/fonts/Deng.ttf`, `system/fonts/times.ttf`, and
  `system/fonts/simsun.ttc`.
- Purpose: deterministic local and CI font packaging; the browser uses Times
  New Roman for Latin text and SimSun as its Chinese fallback.

## Metro Desktop Wallpaper

- Derived resource: `system/resources/wallpaper-metro.bmp`.
- Source: NASA Image and Video Library asset PIA18033,
  `https://images.nasa.gov/details-PIA18033`.
- Credit and usage: NASA; see `system/resources/wallpaper-metro.source.txt`
  and NASA media usage guidelines.

## PortableGL

- Path: `third_party/portablegl`
- Upstream: `https://github.com/rswinkle/PortableGL.git`
- Pinned commit: `7cf39dc1741ea2be60ce3bd327f6e5337f60207f`
- License: MIT; the complete upstream `LICENSE` is staged at
  `/usr/share/doc/reliefos/PORTABLEGL-LICENSE` and in the Developer SDK.

ReliefOS builds the single-header implementation as ABI-v1
`/usr/lib/libportablegl.so.1` and also exposes `libportablegl.a` and the
`reliefos/pgl.h` window wrapper in the Developer SDK. The port fixes the
framebuffer to ABGR32 and depth/stencil to D24S8 and connects presentation to
the ReliefOS pixel-buffer window service. The system build uses PortableGL's
small-memory profile (50,000 output vertices per draw call) to fit the current
user address-space budget. `glxgears` is the bundled GUI smoke test; GLX/X11,
hardware acceleration and multi-threaded contexts are not part of this port.

## Rootfs network databases

`system/rootfs/etc/protocols` and `services` are unmodified Debian netbase v6.4
files, matching the SHA-512 checksums used by Alpine baselayout 3.7.2. Source:
https://salsa.debian.org/md/netbase/-/tree/v6.4 . GPL-2 license, upstream
copyright and source attribution ship in `/usr/share/licenses/netbase`.
