# Boot and Integrity

## Boot flow

ReliefOS boots through GRUB, which loads `kernel.sys` directly:

The host BIOS and EFI tools/modules are built from the same SHA-256-locked
GNU GRUB 2.12 release using its native build system. EFI modules are staged
under `$(O)/grub-efi/lib/grub/x86_64-efi`; PC modules are under
`$(O)/grub-host/lib/grub/i386-pc`. The locked host `grub-mkstandalone`
assembles the EFI application. `GRUB_EFI_DIR`/`GRUB_BIOS_DIR` can explicitly
select another complete compatible platform input tree. The former prebuilt
EFI core does not expose `cmdpath` and has been removed from the source tree.

The default disk GPT contains an unformatted 1 MiB BIOS Boot partition, a
FAT32 ESP, and an ext4 root. The locked i386-pc GRUB embeds its core only in
the protective MBR boot-code area and BIOS Boot partition. Both ISOs have
BIOS platform 0 and UEFI platform 0xef El Torito boot entries.

1. GRUB starts the same `kernel.sys` through Multiboot2 on both firmwares.
   BIOS enters its 32-bit protected-mode trampoline, validates long-mode CPU
   features and sets up paging; EFI amd64 enters through the EFI64 header tag. `kernel.sys` carries its own
   Multiboot2 header and is the single boot image; there is no intermediate
   stage between GRUB and the kernel.
2. GRUB passes the Multiboot2 information, including the framebuffer and
   module tags. On UEFI firmware GRUB keeps boot services available and the
   kernel falls back to the EFI memory map when no Multiboot2 memory map tag
   is present.
3. The kernel imports the installer-root module range from the Multiboot2
   information before physical-memory initialization.

During early boot, normal disk images load components from the FAT32 ESP:

- `/reliefos/kernel.sys`; `/leonos/kernel.sys` is preserved for recovery.
  The automatic rollback entry is shown only when both files are byte-identical.
  Imported older kernels may ignore PARTUUID, so differing rollback kernels need
  manual recovery with only the intended system disk attached.

After the kernel starts, its storage layer mounts the separate ext4 partition
as the normal `/` runtime root. The menu preserves the loader-selected ESP
and reads `/reliefos/config/root.cfg` for Linux `root=PARTUUID=...`. The
kernel resolves that unique GPT identity after disk discovery, rejecting
missing/duplicate/invalid requests instead of mounting another disk. Legacy
`root=/` retains its automatic discovery behavior. Fresh installations use
independent UUIDv4 identities from `/dev/urandom`. The ESP stays separate so
a full root cannot consume boot space.

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

- `$(O)/stage/esp` contains matched kernel, GRUB modules and configuration.
- `tools/build/installer-stage.sh` stages `install/esp`, signed desktop and
  console roots, GNU GRUB guest tools and the shared boot-install CLI.
- `tools/build/images.sh` creates the installer ext4 root;
  `tools/build/iso.sh` publishes both El Torito entries in the same ISO.

GUI and TTY installers default to both firmware loaders, with independent
choices. Updates detect existing modes and retain them when skipped. The
shared `/usr/sbin/reliefos-boot-install` validates whole-disk/ESP ownership,
read-only/mounted/source-target conflicts, BIOS embedding space and payload
before publication. GNU `grub-install --target=i386-pc --force-file-id` owns BIOS
installation; the prebuilt EFI application uses its actual `cmdpath` device.
The common publisher writes the menu last. Firmware/disk writes as a whole
cannot be made power-failure atomic; a tool error is reported as failure.
Old two-partition UEFI disks can update without repartitioning; adding BIOS
requires an existing dedicated BIOS Boot partition and otherwise fails before
publication. The legacy `leonos-grub-installer` command only forwards to this
shared implementation in UEFI-only update mode.

This keeps a freshly installed system consistent with the published boot
layout, and the installer retires any `loader.elf` left over from a
pre-direct-boot layout only after its new `grub.cfg` is published, so an
interrupted update keeps the old menu bootable.

## Trust boundary

GRUB loads `kernel.sys` as the Multiboot2 OS image. There is no
component-hash gate between GRUB and the kernel; this boot path is not a
Secure Boot chain, signature system, or anti-rollback mechanism. Measuring or
verifying the boot chain is outside the current trust model.
