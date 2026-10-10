# ReliefNT Drivers

## Layout

All driver source code lives in the kernel submodule under `kernel/reliefnt/drivers/`.
Every driver is linked into `kernel.sys` itself; there are no loadable driver
modules and no driver files on disk.

- `kernel/reliefnt/drivers/bootstrap`: console, framebuffer, VGA, EFI filesystem, storage, and
  USB UHCI/HID implementations that are linked into `kernel.sys`.
- `kernel/reliefnt/drivers/mouse`, `kernel/reliefnt/drivers/serial`, `kernel/reliefnt/drivers/e1000`, `kernel/reliefnt/drivers/ac97`,
  `kernel/reliefnt/drivers/es1371`, and `kernel/reliefnt/drivers/hda`: device drivers compiled
  into `kernel.sys` alongside the bootstrap drivers (see `KERNEL_SOURCE_DIRS`
  in `kernel/reliefnt/mk/kernel.mk`).

## Module ABI

Each driver file exports a `const struct reliefos_driver_module` descriptor
with a unique per-driver symbol name (for example `mouse_driver_module`). The
descriptor uses ABI version `RELIEFOS_DRIVER_ABI_VERSION`, identifies the
module, declares its driver kind, and supplies `init` and optional `fini`
callbacks. Drivers receive `struct reliefos_driver_kernel_api`; they do not
call arbitrary kernel subsystems directly. The API table binds a mouse input
provider, serial console provider, e1000 link provider, or audio card.

## Startup

During boot the driver manager initializes every built-in driver in a fixed
order (serial first so console output moves off the early COM1 path, then the
remaining drivers). A failed init is rolled back (audio STOP, resource
release, `fini`) and retried once, then recorded as failed while boot
continues. There is no runtime load, unload or boot-disable action: the frozen
driver control ioctl reports `-EOPNOTSUPP` for every action.

## USB HID

The bootstrap USB layer scans PCI UHCI (USB 1.1) controllers during kernel
startup. It resets each root port, enumerates standard HID boot-protocol
interfaces, and polls interrupt endpoints for keyboards and mice. A single
level of USB hub is also configured so multiple devices can share a root port.
Keyboard usages are translated to the existing set-1 keycodes, while mouse
reports are published to the existing relative pointer event queue. EHCI,
xHCI, USB storage, generic (non-boot) HID report parsing, and runtime hot-plug
are not implemented yet.

## Management and Trust Boundary

`drvmgr.elf` lists the built-in drivers, their ABI versions, loading state and
errors. Every logged-in user may read this state. The management view is
read-only: drivers are part of the kernel image, so there is nothing to load,
unload or disable at runtime, and the frozen driver control ioctl reports
`-EOPNOTSUPP` for every action.

## Audio ABI probe boundary

The Linux v6.14 sound UAPI export is checked by `tools/test_linux_audio.py` and
the standard-libc source probe `tools/tests/audio_guest_test.c`. The probe
supports ABI layout, PCM playback/capture, mapped lifetime, and control
snapshot/restore modes; its host gate runs ABI, invalid-argument, and
transport-failure checks until the HDA module and guest image are available.
The supported ioctl, access, state, errno, mmap, disconnect, and multi-card
contract is recorded in `docs/audio-abi-support.json`. No DMA address is
printed by the probe. Guest I/O, HDA module loading, upstream ALSA, and
physical speakers/microphones remain separate validation gates.

## HDA integration evidence

The runs below predate built-in driver linking; at that time the drivers were
loadable `.drv` modules published in VMDK, Live ISO and Installer ISO products. An isolated QEMU q35
guest with Intel HDA, a duplex codec, four CPUs and MSI enabled has loaded
the module and registered a codec card. Its standard-libc probe runs through
normal OpenRC startup. Playback, mapped lifetime, control restoration, and
83 native SysV SHM checks passed. Actual WAV PCM contains separate 1000/2000 Hz
stereo tones for two seconds. An isolated ALSA-file input backend also yielded
4096 captured stereo frames matching injected 400/800 Hz samples. PC/q35,
one/four CPUs, MSI on/off, ICH9 and a second codec card have passing runs under
`out/audio-hda/qa/hw5`, `hc2`, `hc3`, `hc4`, and the latest `hc11`. Unfinalized WAV container lengths
are repaired in separate normalized files; original PCM artifacts are retained.
The initial PREPARE failure remains recorded under `hw1`. Copied modules are
rejected before init when their descriptor name is already loaded. Freed MSI
vectors remain retired until reboot; exhausting the fixed sixteen-vector
budget permits HDA to use its polling service. Task-context fatal cleanup, capability-based jack controls, and bounded
Auto-Mute group/stream transactions have host fault-injection evidence.
Auto-Mute is exposed only for compatible independent stereo speaker/headphone
groups; switching retains the PCM lease and preserves user gain/mute. Ordinary
prepare also preserves published user volume. The latest guest run verifies
playback/capture/control/lifetime but does not inject jack or fatal faults.
AC97 continuous 48 kHz stereo waveform verification passed under `qa/ac-full5`:
95999 active frames, maximum period error one. ES1371 was verified under
`qa/esvm10` using a private VMware SoundLib/ALSA backend; the ES1371 guest
loaded successfully, completed all guest checks, and produced the same
continuous stereo tones. This proves the configured private guest/backend path,
not physical ES1371 hardware. The exact HDA, AC97 and ES1371 guest module hashes
match current generated driver products. Physical-device acceptance and the
upstream user applications remain pending.

`QEMU_SOUND_DEVICE` selects `hda` (default), `ac97`, `es1371`, or `none` for
the root run targets. The v1 OSS adapter holds an exclusive backend-generation
lease and advertises only stereo S16_LE playback, with GETCAPS returning zero.
It exposes neither capture nor RESET/pause/pointer capabilities as ALSA support.
AC97 guest writes and SYNC passed under `qa/ac-full2` and `qa/ac-full3`; application
queue accounting excludes driver-generated idle silence. Their WAV artifacts
still have gaps/repetitions, so continuous playback quality is not accepted.
The current QEMU `ES1370` model does not match the ES1371 driver's PCI identity;
`qa/es1` records that failed module check, not an ES1371 guest acceptance.
Current source, product and runtime identities are recorded in
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-12-runtime-checkpoint-6.json`.
