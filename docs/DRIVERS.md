# ReliefNT Driver Modules

## Layout

All driver source code lives in the kernel submodule under `kernel/reliefnt/drivers/`.

- `kernel/reliefnt/drivers/bootstrap`: console, framebuffer, VGA, EFI filesystem, storage, and
  USB UHCI/HID implementations that are linked into `kernel.sys`.
- `kernel/reliefnt/drivers/mouse`, `kernel/reliefnt/drivers/serial`, `kernel/reliefnt/drivers/e1000`, `kernel/reliefnt/drivers/ac97`,
  `kernel/reliefnt/drivers/es1371`, and `kernel/reliefnt/drivers/hda`: loadable implementations built as `mouse.drv`,
  `serial.drv`, `e1000.drv`, `ac97.drv`, `es1371.drv`, and `hda.drv` (see `DRIVER_NAMES`
  in `mk/boot.mk`).

The normal image, normal ISO, installer runtime root, and installed ESP place
loadable modules in `/usr/lib/reliefos/drivers`; ESP modules use `/drivers`.

## Module ABI

A `.drv` is an unsigned x86_64 ELF64 `ET_REL` module. The kernel prefers the
`reliefos_driver_module` descriptor and still accepts the legacy
`leonos_driver_module` symbol. Both use ABI version
`RELIEFOS_DRIVER_ABI_VERSION`, bounded allocatable sections, and supported
local relocations. Modules receive `struct reliefos_driver_kernel_api`; they
do not link directly against arbitrary kernel symbols.

The descriptor identifies the module, declares its driver kind, and supplies
`init` and optional `fini` callbacks. The kernel uses the API table to bind a
mouse input provider, serial console provider, or e1000 link provider.

## Startup and Configuration

After `/` is mounted, the kernel scans the direct files in `/usr/lib/reliefos/drivers`.
Every valid, enabled `.drv` is loaded in deterministic directory order. A
failed module is retried once, then recorded as failed while boot continues.

`/etc/reliefos/drivers.conf` is optional. It uses UTF-8 text with a `version=1` line
and one `disabled=<file>.drv` line per module excluded from automatic startup.
Absent entries are enabled by default.

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

`drvmgr.elf` lists driver files, ABI versions, loading state, errors, and
boot-disable state. Every logged-in user may read this state. Loading,
unloading, forced unloading, rescanning, and changing boot enablement require
an administrator session; the kernel enforces this before dispatching control
requests.

Forced unloading removes the module's service binding and runs its cleanup
callback. A failed audio STOP or reported unsafe DMA teardown retains the
module image and its resources for a later retry; only safe cleanup frees them.
Removing `mouse.drv` stops mouse input;
removing `serial.drv` removes serial console output; removing `e1000.drv`
stops network links and clears active socket state. A module can be loaded
again from its file without restarting.

`.drv` files execute in Ring 0 and are intentionally not signed or hashed in
this version. Only trusted administrators should be allowed to write
`/usr/lib/reliefos/drivers` or modify its contents.

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

The ordinary root build publishes `hda.drv` alongside the existing five
modules in VMDK, Live ISO and Installer ISO products. An isolated QEMU q35
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
