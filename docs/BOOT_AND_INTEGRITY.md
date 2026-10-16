# Boot and Integrity

## Boot flow

ReliefOS boots through GRUB, which loads `kernel.sys` directly:

The image build uses the tracked GRUB 2.12 EFI kernel and modules in
`boot/grub_modules_x86_64-efi`. The host `grub-mkstandalone` assembles these
inputs into each new EFI image. `GRUB_EFI_DIR` can explicitly select another
validated module set. Unpatched GRUB 2.14 EFI modules fault while patching
read-only relocator stubs during Multiboot2 boot; upstream fixes this in
[commit 62289192](https://github.com/rhboot/grub2/commit/62289192bb45bd90f5917c53fd29083d9a9672b1).

1. GRUB starts `kernel.sys` through Multiboot2. `kernel.sys` carries its own
   Multiboot2 header and is the single boot image; there is no intermediate
   stage between GRUB and the kernel.
2. GRUB passes the Multiboot2 information, including the framebuffer and
   module tags. On UEFI firmware GRUB keeps boot services available and the
   kernel falls back to the EFI memory map when no Multiboot2 memory map tag
   is present.
3. The kernel imports the installer-root module range from the Multiboot2
   information before physical-memory initialization.

During early boot, normal disk images load components from the FAT32 ESP:

- `/reliefos/kernel.sys` (the legacy rollback entry boots `/leonos/kernel.sys`
  on old disks)

After the kernel starts, its storage layer mounts the separate ext4 partition
as the normal `/` runtime root. The ESP stays separate so a full root cannot
consume UEFI boot space.

Installer and live ISOs boot the same kernel and pass the installer root as a
GRUB module:

- `multiboot2 /reliefos/kernel.sys`
- `/install/root.fat` with module tag `reliefos-installer-root`
  (legacy `leonos-installer-root` remains accepted)

The installer root remains resident for the installer session. It is accessed
through a shared supervisor-only high direct map so user page tables cannot
replace its low physical placement. The VM must provide enough RAM for GRUB to
load the whole module; a 400 MiB root is supported with 1 GiB or more of guest
memory.

GRUB chooses module placement and keeps modules clear of the kernel image,
while `kernel.sys` has a fixed physical `PT_LOAD` destination. The kernel
reserves its own image range from the linker-provided `__kernel_start` and
`__kernel_end` symbols during memory initialization, and imports the
installer-root module range before physical-memory initialization, so the
Multiboot range can be reclaimed without corrupting the FAT filesystem.

## GRUB framebuffer boot log

After GRUB supplies the Multiboot2 framebuffer tag, the kernel bootstrap
console renders its startup log on that framebuffer using the built-in PSF
font, replaying the log collected before the framebuffer was enabled. Serial
logging remains mirrored and stays available when GOP/framebuffer output is
unavailable.

The panel uses Metro blue by default and switches to the persisted Win95 or
Metro theme after the kernel reads the boot theme from
`/reliefos/config/display.conf` on the ESP (with the old
`/leonos/config/display.conf` path accepted for existing disks). It requires a
32-bit linear framebuffer.

`bootlog-pause=1` also enables the log panel, repeats the SVGA3D initialization
summary after the other boot services, and waits for Enter before launching
user processes. The installed system's `with boot log` and `SVGA II 3D test`
entries and the installer's `Enable boot log screen` entry enable this pause.
The normal boot entries and `bootlog=1` alone still continue automatically.
IRQ/timer input remains active while paused; queued input, mouse events, and
key releases are ignored. Press Enter in the virtual machine to resume boot.

Kernel log levels and formatting are shared by every built-in driver. Level
labels use muted ANSI colors by default. Append `logcolor=0` to the
`multiboot2 /reliefos/kernel.sys ...` line to disable those escapes from the
first boot message; `logcolor=1` enables them explicitly. Repeated valid values
use the last one. This setting does not disable userspace or debugger colors.
Framebuffer replay interprets the same ANSI labels as serial output; the VGA
text fallback strips the escape sequences.

The first text-console handoff happens after the final `boot complete` log.
tty1 adopts the existing framebuffer text and cursor without clearing or
replaying over the screen, so PID 1 and OpenRC output continues immediately
below the kernel messages. The retained boot log becomes part of tty1's text
history and is replayed when returning from another VT or graphical mode.
Reactivating the already-visible text VT does not clear it. Actual VT changes,
graphical ownership changes and explicit userspace clear sequences still work.

## Installer compatibility

The installer writes target disks through AHCI, legacy IDE/PATA PIO, or NVMe.
NVMe namespaces with 512-byte logical sectors are supported as boot and install
targets. In VirtualBox, attach the destination VDI through a SATA/AHCI,
NVMe, or default PIIX4 IDE controller. IDE/ATAPI optical media is read-only
and is supported for ISO reads.

The installer payload is built from the same matched runtime staging tree:

- `build/esp` contains the kernel, resources, config, and
  userland applications for the installed system.
- `tools/make_installer_root.py` splits `build/esp` into `install/esp` (the
  FAT32 ESP boot subset) and `install/root` (the ext4 runtime root) inside
  `build/install/root.fat`.
- `tools/make_installer_iso.py` stages top-level installer copies of
  `system/kernel.sys` and `install/root.fat`.

This keeps a freshly installed system consistent with the published boot
layout, and the installer retires any `loader.elf` left over from a
pre-direct-boot layout only after its new `grub.cfg` is published, so an
interrupted update keeps the old menu bootable.

## Trust boundary

GRUB loads `kernel.sys` as the Multiboot2 OS image. There is no
component-hash gate between GRUB and the kernel; this boot path is not a
Secure Boot chain, signature system, or anti-rollback mechanism. Measuring or
verifying the boot chain is outside the current trust model.
