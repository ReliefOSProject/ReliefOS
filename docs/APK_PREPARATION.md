# APK-managed LeonOS roots

> Historical snapshot from 2026-09-14, before the ReliefOS package and brand
> migration. Package names, kernel identity, and validation output below record
> that earlier state; current package names and identity are documented in
> `docs/BUILDSYSTEM.md` and `docs/branding-compatibility.md`.

LeonOS now ships the unmodified Alpine apk-tools-static 3.0.8-r0 executable as
`/sbin/apk`. Normal ISO, VMDK and installer builds use real signed APK
transactions to populate the root filesystem and `/lib/apk/db/installed`.
The database is never synthesized from the ownership inventory.

## Package ownership

Existing payloads are packaged into local `leonos-*` packages. This is actual
APK ownership of the current binaries, not a claim that Alpine built them.
The default root includes the patched musl runtime and development files,
authentication stack, BusyBox, official Alpine findutils, file, less and Vim,
desktop, storage tools and custom Fastfetch. Optional build selections change
the package set.

`configs/apk-ownership.json` still records the eventual upstream handover
decisions. The packaged copy reports `installed-by-upstream-apk`. Remaining
files from the distribution staging root are deliberately owned by
`leonos-base`, including notices, configuration and auxiliary build payloads.
BusyBox's `ar` and `strings` fallback links are explicitly script-managed,
following Alpine's trigger model: binutils may own these paths, and removing it
restores only missing links. The BusyBox executable remains package-owned.
The package advertises its actual `/bin/sh` provider so shell script
dependencies do not pull in a conflicting second BusyBox. `/bin/bash` is
reserved for upstream GNU Bash; the former ash alias is no longer shipped.
Upstream BusyBox `add-shell` and `remove-shell` register shell packages in
`/etc/shells` during their original installation/removal scripts.
This coarse package split can be refined in a future signed package migration.

Nano, TinyCC and Lua are not bundled or built by LeonOS; install the upstream
packages with `apk add nano`, `apk add tcc` or `apk add lua5.4` when needed.
file, less and Vim and their runtime dependencies are pinned as signed Alpine
APKs in `configs/dependencies.lock.json`. `make fetch` downloads the binary
archives; rootfs packaging verifies their signatures and installs them offline
with their original package names and ownership. No local `leonos-file`,
`leonos-less`, `leonos-vim`, `leonos-lua` or `leonos-tcc` packages are produced.
After `make O=out rootfs`, run
`sh tests/integration/test-official-tools.sh out` to check installed ownership,
original executable bytes and absence of the retired local packages.

The CA bundle has its own
`ca-certificates-bundle` package. Its local version identifies a LeonOS build;
signed Alpine upgrades can replace it without overwriting the trust package.

Actual ELF DT_NEEDED and SONAME/real library filenames determine local runtime
dependencies and library providers. The real musl loader also has the
`/lib/libc.musl-x86_64.so.1` filename required by Alpine libraries. No dummy
Alpine package versions, blanket `provides`, or `replaces` declarations are used.

The optional Xorg desktop uses the same offline transaction path. Alpine v3.24
x86_64/musl packages for `xorg-server`, `xorg-server-common`, `twm`, `xdm`,
`xterm`, `xf86-video-fbdev`, `xf86-input-evdev`, `xkeyboard-config`,
`font-cursor-misc` and `font-misc-misc`, together with every dependency and
shared-object provider, are pinned in `configs/dependencies.lock.json` with
`feature=xorg`. Entries without that field are `feature=base`. `apk-stage.sh`
reads the raw `/etc/reliefos/desktop-backend` marker and asks `reliefos-deps`
for each entry's feature; a native root installs only base entries, while an
Xorg root installs both sets. The package cache may contain both sets, but a
native managed root never does. Missing archives name the dependency and point
to `make fetch`; normal builds never resolve packages from the network.

`leonos-fastfetch` owns the custom LeonOS Logo binary and depends on
`!fastfetch`. It is installed in world. An ordinary `apk add fastfetch@alpine`
cannot replace it; root can deliberately remove the local package first.
A negative world entry alone would not enforce this conflict.

Python, GCC/binutils and TCC remain absent from the default payload. Generic
tools and critical runtime packages are currently real local APKs; transferring
them to upstream Alpine packages is a separate operation requiring conflict and
ABI checks. Do not use `--force-overwrite` to install overlapping upstream
packages. Dependencies requiring an exact Alpine musl/package revision are not
guaranteed to resolve against the local patched runtime.

`leonos-musl-dev` is installed by default and retained in the offline repository.
It contains the headers, startup objects and
static libraries from the exact musl sysroot used by the runtime and depends on
that `leonos-musl` version. The builder checks runtime identity before packaging.
It provides the unversioned `musl-dev`/`libc-dev` interfaces, not an invented Alpine musl
version. New installations can use `apk add gcc make` directly; asking for Alpine's exact-version
`musl-dev` remains a different request. The existing libxcrypt header and library
retain their own ABI. `libssp_nonshared.a` forwards stack-protector failures to
musl, matching the support library required by Alpine GCC's default specs.
Installer repackaging preserves this package's exact file ownership, including
the development `libc.so` symlink. Existing installations that do not have the
development package can add it with `apk add leonos-musl-dev`.

## Repositories and trust

The image contains a signed offline repository at
`/usr/share/leonos/apk/repository/packages.adb`. `/etc/apk/repositories` lists
it first, followed by the Tsinghua mirrors of Alpine v3.24 main/community for
native x86_64:

```text
https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.24/main
https://mirrors.tuna.tsinghua.edu.cn/alpine/v3.24/community
@testing https://mirrors.tuna.tsinghua.edu.cn/alpine/edge/testing
```

The release branch remains v3.24. The same repositories retain `@alpine` aliases for existing world entries.
Ordinary `apk add NAME` can select upstream packages without a tag.
Testing exists only on the rolling edge branch and is opt-in: use
`apk add hyfetch@testing` for the native x86_64 HyFetch package. Its current
dependencies resolve against v3.24 and the local musl runtime. The `@testing`
tag keeps unrelated stable installs and upgrades from selecting testing
packages by default; edge main/community are not enabled. Future testing
packages may require newer dependencies than v3.24 provides.
Existing installations can add the tagged line above to `/etc/apk/repositories`
and run `apk update`; installer upgrades preserve locally edited configuration.
`/etc/os-release` remains LeonOS, with the Alpine non-usr-merge layout.

Alpine signing keys and the Web PKI bundle have separate roles. Both signature
and TLS verification remain enabled. No host proxy configuration is shipped.
The explicit `cache-dir /var/cache/apk` setting enables APK package reuse as well
as index caching. Merely creating the fallback index directory does not enable
upstream apk's package-cache policy.

The build bootstraps the fixed official apk archive with SHA-256
`c8e2c88c13ba12a12269b79a3543e1190ff8c0ab0beb32b58cadfd5881c619e3`.
It additionally checks the executable hash and its official detached RSA-SHA256
signature. The GPL license and source/build attribution are shipped under
`/usr/share/licenses/apk-tools`. The upstream static build has no embedded
command help; this does not change transaction semantics.

The default local signing key is generated once at
`~/.local/share/leonos/apk-signing/key.pem`, mode 0600, outside the repository.
Only its public key enters the image. Preserve this private key for subsequent
updates of images built with it. `APK_SIGNING_KEY` can select a maintained
release key. A new key is not automatically trusted by an installed system.
Changing the selected key intentionally requires a separate authenticated key
rotation or a fresh install; the updater never imports the media's key into
the target trust store.

## Build and installation

`make apk-repo` builds the normal staging payload, makes signed
APKv3 packages/index with upstream `apk mkpkg/mkndx`, and installs them into
`$(O)/rootfs/managed` using upstream `apk add`. When available, host user namespaces
map ownership to root without modifying the host root or requiring sudo. On
runners that prohibit user namespaces, the build uses the installed `fakeroot`
compatibility path instead. Normal image targets depend on this managed root.

The build manifest at `build/apk/normal/manifest.json` records package versions,
files and signing identity. Local versions identify build snapshots, not
upstream version claims. The original staging inventory remains an audit aid.

Installer runtime and installed payload have different desktop/runtime policy
binaries. Each is repackaged *after* those substitutions, then populated through
apk again. Their database checksums therefore describe their own final
executables. Image/account setup may subsequently change protected configuration,
just as administrator changes do on an installed Linux system.

ESP-only EFI/GRUB/loader/kernel payloads are handled by the boot-image publisher.
They are not recorded as files in a rootfs APK and are not removed by rootfs
package operations. Volatile runtime directories and package caches/databases
are not recursively packaged as application data.

## Installer updates

Updates run `/usr/lib/leonos/leonos-apk-update` against the mounted target,
using its existing trust keys and the media's signed repository. The updater
upgrades only installed `leonos-*` packages; a removed optional package is not
reinstalled. Alpine packages, world selections and locally edited configuration
are retained under apk's normal rules. New defaults may appear as `.apk-new`.

The former directory overlays for root libraries, programs, drivers and docs
are no longer used during an update. The installer now confirms a package
update instead of offering the old file-copy application selection. Boot files
are published only after the root package transaction succeeds.

Systems without an APK database are rejected with a reinstall message. No
automatic conversion of legacy roots is attempted. A failing transaction does
not become success and does not trigger boot-file publication. As with upstream
apk, package upgrades are not whole-filesystem atomic rollback transactions.

## Commands

After boot, as root:

```sh
apk update
apk info
apk info --who-owns /usr/bin/fastfetch
apk add zlib
apk del zlib
apk add gcc make
apk add hyfetch@testing
gcc hello.c -o hello
./hello
```

An installed local package can be removed explicitly before an upstream
replacement, but inspect its reverse dependencies and shared paths first.
Never remove the working libc/authentication/base merely to make the solver
accept a package.

## Verification

The 2026-09-13 testing-repository regression uses unmodified x86_64
`hyfetch-2.0.5-r0` and `bash-5.3.9-r1`. A clean host root transaction and QEMU/KVM
(one socket, two cores, e1000) both install without BusyBox or Bash ownership
conflicts. `tools/test_apk_qemu.py --testing-only` passes 28 checks: HTTPS and
index signatures, installation/removal, `/etc/shells` scripts, Bash arrays,
HyFetch version/help, retained custom Fastfetch ownership and real `MAP_STACK`
guard-page/signal-stack behavior. Evidence: `build/apk-testing/qemu-test.log`.
The same raw mapping test passes on host Linux. Linux v6.12 accepts `MAP_STACK`
and maps it to `VM_NOHUGEPAGE`; NTCLKS user mappings already use only 4 KiB pages.
This fixes Rust's startup failure without modifying the Alpine binary.

The 2026-09-14 follow-up reproduces and repairs the interactive failure with
the unchanged Alpine HyFetch binary. Its terminal-color query registers PTY
stderr with epoll: the separate PTY descriptor table was rejected with EBADF,
and a 1024-event output buffer was wrongly limited to the 64 registered-entry
capacity. Readiness probes also cleared the deadline, causing finite waits to
restart indefinitely. PTYs are now accepted, output capacity uses Linux's
`INT_MAX / sizeof(struct epoll_event)` bound, and finite waits retain their
deadline and return zero after expiry when no event is ready.

Rust's child-output capture additionally requires generic `ioctl(FIONBIO)` on
pipes. The generic VFS path now changes the shared open description's nonblocking
flag, preserving descriptor-local CLOEXEC and other status flags. Terminal
answers OSC 10/11 and primary device-attribute queries, consumes split and
oversized OSC strings safely, and renders semicolon-form RGB/256-color SGR.

The Linux userspace personality now returns `Linux` from `uname.sysname` and
`/proc/sys/kernel/ostype`. The embedded neofetch script otherwise rejects
`ntclks` as an unknown OS. This is an ABI personality name, not a claim that
NTCLKS is the Linux kernel: `uname.release` remains the actual NTCLKS version,
`/proc/version` retains the NTCLKS identity, and `/etc/os-release` remains LeonOS.
Fastfetch's Kernel name consequently reads `Linux` with the NTCLKS version.

`tools/test_hyfetch_qemu.py` stages the real signed `hyfetch@testing` package and
its dependencies, runs the same static musl raw-syscall probe on host Linux and
NTCLKS, and drives the default wizard through the Live desktop's Terminal.
It checks `sudo hyfetch --config-file /tmp/hyfetch-wizard.json`, persisted JSON
with automatic background detection, and a normal `hyfetch` run using the
packaged LeonOS defaults. The raw probe covers PTY readiness,
1024-event capacity, repeated finite timeouts, one-shot rearming, FIONBIO's
dup-shared flags, actual empty-pipe EAGAIN, and invalid-fd/pointer/O_PATH errors.
Evidence and the diagnostic ISO are under `build/hyfetch-qemu`.

`leonos-fastfetch` owns `/usr/share/fastfetch/leonos-ascii.txt` (the same LeonOS
art as its pinned binary) and `/etc/skel/.config/hyfetch.json`. Standalone
images seed the root/test homes; the installer seeds root and the chosen user
without replacing existing configurations. HyFetch applies its RGB palette
to this custom ASCII instead of falling back to the generic Linux logo.
Existing users can set `custom_ascii_path` to that absolute path in their own
HyFetch JSON, or pass `--ascii-file /usr/share/fastfetch/leonos-ascii.txt` when
no custom path is configured. No HyFetch binary or APK patch is required.

LeonOS-logo validation on 2026-09-14: five Fastfetch packaging tests, four
installer setup tests and three APK ownership tests passed. `build.py run
apk-root` completed with zero errors; both new files belong to
`leonos-fastfetch` in the generated ownership inventory. QEMU/KVM with the
signed Alpine HyFetch package passed the fresh wizard and normal default-logo
run (both exit 0), and the raw epoll/PTYS probe reported zero failures.
`build/hyfetch-qemu/terminal-hyfetch.png` shows the recolored LeonOS art;
`logo-build.log`, `logo-qemu.log` and `guest-serial.log` in that directory retain
the build and guest evidence. Installer config initialization was covered on
the host, not by a complete install/reboot cycle in this logo follow-up.

The test exercises a Live root session. An earlier image with the installed
login marker stalled while opening a PAM session; that separate failure is
preserved in `build/hyfetch-qemu/login-stalled.log` and is not certified by this
test. VMware and installed-system login were not verified in this follow-up.
The pre-existing whole-UAPI header check still fails
because `kernel/ntclks/include/uapi/leonos/net_control.h` includes `leonos/net.h` outside the
UAPI include root; the changed `linux/mman.h` passes standalone C/C++ checks.

The TCP/e1000 receive-path correction is covered by `tools/test_network.py`,
`tools/test_e1000.py` and `tools/test_tcp_download_qemu.py`. In an isolated local
HTTPS QEMU fixture, downloading the unmodified 60,496,469-byte GCC APK took
13.65 seconds; installing `gcc leonos-musl-dev make` and their dependencies took
29.20 seconds, followed by successful compilation and execution of a stdio
program. These are local-fixture results, not promises about public mirror or
VMware speeds. See `docs/NETWORK_STATUS_2026-09-13.md` for evidence and scope.

```sh
make test-apk
python3 tools/test_apk_qemu.py
python3 tools/test_apk_qemu.py --testing-only
python3 tools/test_terminal.py
python3 tools/test_linux_ioctl_cloexec.py
# Requires current kernel, terminal app and apk-repo build outputs.
python3 tools/test_hyfetch_qemu.py
python3 tools/test_alpine_runtime_qemu.py
python3 tools/test_apk_qemu.py --package-cache out/x86_64/release/packages/apk/cache
make iso
python3 tools/test_apk_layout.py --images
```

The host transaction regression checks signed installation and actual file
ownership, empty-directory modes, unsigned rejection, Fastfetch conflict,
configuration preservation, and the real updater's preservation of removed
optional packages.

The QEMU test uses an ordinary Linux/musl probe and the unchanged apk binary.
It exercises signed local indexes, install scripts, triggers, file modes,
upgrades with edited configuration, removal, unsigned rejection, Fastfetch
conflict, an unchanged Alpine zlib package and a dynamic compression roundtrip,
plus HTTPS retrieval and signature verification of Alpine indexes. Host downloads
follow the caller's proxy environment; the QEMU guest defaults to direct access.
`--guest-proxy URL` optionally sets a proxy reachable from QEMU in the test fixture
only. No proxy address is built into the test or production images.
Evidence is under `build/apk-qemu`.

The Alpine runtime test installs signature-verified, unmodified GCC/binutils,
make, nano, jq, OpenSSL and curl into an isolated host root, then executes them
on NTCLKS in QEMU. It checks object compilation, dynamic/static linking and
execution of generated stdio/pthread/TLS programs. It also tests delivery and
reaping of both default and caught SIGSEGV at instruction address zero.
Evidence is under `build/alpine-runtime`. Host package staging in this test does
not substitute for the separate guest APK transaction test.
The optional `--package-cache` seeds only the final GCC installation from real
APK cache files; the earlier HTTPS/index/download tests remain online. This
separately measures installation behavior when large remote downloads exceed
the test's 300-second GCC transaction limit. A cached run does not certify
uncached download throughput.

The GCC startup fix distinguishes ELF type (ET_EXEC/ET_DYN load bias) from
PT_INTERP. Both executable types enter their interpreter when present, with
AT_ENTRY describing the main program and AT_BASE the interpreter. A fault at
RIP zero reaches signal delivery before validating a return frame; it no longer
retries the fault indefinitely.

Passing these tests is not certification of every Alpine package, its init
system, scripts or kernel dependencies. VMware needs separate verification.

## Upstream references

- [apk package metadata](https://gitlab.alpinelinux.org/alpine/apk-tools/-/blob/v3.0.8/doc/apk-package.5.scd)
- [Signed package creation](https://gitlab.alpinelinux.org/alpine/apk-tools/-/blob/v3.0.8/doc/apk-mkpkg.8.scd)
- [Repository index creation](https://gitlab.alpinelinux.org/alpine/apk-tools/-/blob/v3.0.8/doc/apk-mkndx.8.scd)
- [Repository syntax](https://gitlab.alpinelinux.org/alpine/apk-tools/-/blob/v3.0.8/doc/apk-repositories.5.scd)
- [World constraint replacement](https://gitlab.alpinelinux.org/alpine/apk-tools/-/blob/v3.0.8/src/app_add.c)
- [Official build recipe](https://gitlab.alpinelinux.org/alpine/aports/-/blob/3.24-stable/main/apk-tools/APKBUILD)
- [Alpine musl development support library](https://gitlab.alpinelinux.org/alpine/aports/-/blob/3.24-stable/main/musl/APKBUILD)
- [Linux v6.12 ELF loading](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/binfmt_elf.c?h=v6.12)
