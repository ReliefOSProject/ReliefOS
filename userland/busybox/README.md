# ReliefOS BusyBox profile

ReliefOS builds upstream BusyBox 1.36.1 as a static `/bin/busybox` with a broad
applet profile. The production build exports `busybox.links`; both that list
and the resolved Kconfig configuration are checked before publishing the
executable. Run `/bin/busybox --list` to inspect the actual binary, or invoke an
applet directly:

```sh
/bin/busybox hexdump -C /etc/os-release
/bin/busybox free -m
/bin/busybox top -b -n 1
```

The profile adds process and filesystem diagnostics (`df`, `free`, `top`,
`dmesg`, `lsof`, `pgrep`, `pstree`), text tools (`hexdump`, `od`, `tree`, `bc`,
`dc`), archive tools (`cpio`, `unzip`, `bzip2`, XZ/LZMA decompression, `lzop`),
network tools (`ping`, `ping6`, `nc`, `netstat`, `traceroute`, `telnet`, FTP/TFTP tools), and
administration applets (`crond`, `crontab`, `mdev`, module tools). `ls`, `cp`,
`diff` and `tar` include their usual extended options. BusyBox's `xz` and
`lzma` applets decompress only; they do not provide compression.

Compilation does not establish that every Linux device, module, network or
namespace interface exists in ReliefNT. These applets report errors when the
required kernel interface is unavailable. Building a daemon does not enable a
service: the existing OpenRC configuration remains responsible for startup.

## Command ownership

Commands supplied by another system package or a native application are
excluded from this profile:

- GNU findutils supplies `find` and `xargs`; GNU coreutils supplies only `dd`.
- util-linux supplies `mount`, `umount`, `fdisk`, `sfdisk`, `blkid`, `lsblk`,
  `fsck`, `su` and `runuser`.
- e2fsprogs, dosfstools and exfatprogs supply their matching filesystem
  formatters/checkers; e2fsprogs also owns `chattr`, `lsattr` and `tune2fs`.
- PAM, sudo and shadow own login, authentication and account management.
- ncurses supplies `clear` and `reset`; xterm supplies `resize`.
- The official Alpine packages supply `less` and `xxd`. GNU `wget` is supplied
  with the Xorg backend; the BusyBox wget/TLS implementations remain disabled.
- OpenRC supplies `start-stop-daemon`.

BusyBox supplies `/bin/ping` and `/bin/ping6` as command-line applets using
ICMP sockets. Raw sockets require the kernel's applicable permissions; no
setuid mode is added to BusyBox. IPv6 requires support in the target kernel.
For example, run `ping -c 3 -W 2 10.0.2.2` from a terminal.

Rootfs staging inspects files, symlinks and selected directory trees in its
plan before adding BusyBox links. A command already provided in any of
`/bin`, `/sbin`, `/usr/bin` or `/usr/sbin` takes precedence across all four
locations. APK staging also preserves the file ownership of the selected
upstream archives. `ar` and `strings` remain optional BusyBox fallbacks: the
package trigger adds their links only when no existing provider is present.

## Shell and compatibility limits

`/bin/sh` remains BusyBox `ash`, using the upstream MMU
`fork`/`pipe`/`dup2`/`execvp`/`waitpid` path. Hush and Bash aliases are disabled.
Shell command lookup follows `PATH`; standalone applet execution and applet
preference are disabled so external tools retain their command ownership.

Interactive ash job control (`jobs`/`fg`/`bg`) stays disabled while ReliefNT's
SIGTTIN/SIGTTOU stop-and-continue protocol is incomplete. Pipelines,
redirections, background process creation, Tab completion, and fancy prompts
remain enabled. Persistent history, reverse history search and full
Unicode/locale support retain their existing disabled state.

The vi regex extension requires GNU regex interfaces unavailable in musl, so
vi retains ordinary search and its other editing features. BusyBox 1.36.1's
`tc` requires obsolete CBQ definitions absent from the pinned Linux headers
and is disabled. The legacy `/linuxrc` entry is also excluded: generated
applet links must use the four executable directories above.

BusyBox is GPL-2.0-only; its license is staged under
`/usr/share/licenses/busybox/`.
