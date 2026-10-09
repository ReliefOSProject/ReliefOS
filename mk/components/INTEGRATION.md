# Userland migration integration and verification

Include `mk/userland.mk` after runtime/SDK and third-party fragments; it includes
`mk/components/graphics.mk` itself. Parent has integrated the root Makefile.
Host tool rules are self-contained in the fragment. `tools` may additionally
depend on `$(RELIEFOS_COMPONENT_TOOL)` and `$(RELIEFOS_GEARS_TOOL)`.

## Interfaces

- `RELIEFOS_COMPONENTS_ENABLED`, `RELIEFOS_COMPONENTS_DISABLED`,
  `RELIEFOS_COMPONENT_APPS` come from `configs/components.toml` plus O/config/.config.
  Required components remain enabled; dependency closure follows the old resolver.
- Apps: `O/userland/NAME.elf`, aliases `app-NAME` for enabled project applications.
- Installer: `O/userland-installer/gptinit.elf`; aggregate `installer-userland`.
- Static loader-error GUI: `O/userland/dynlinkerror.elf` (no dynamic section).
- Graphics exports: `PORTABLEGL_SO`, `PORTABLEGL_ARCHIVE`, `GLXGEARS_SOURCE`,
  `STARDUSTUI_ARCHIVE`. Stardust examples are owned by this fragment too.
- Remaining external app outputs from upstream_migration:
  `O/userland/{fastfetch,pleditor}.elf`. No fallback or silent skip exists.
- Other enabled tool/library targets must be attached by their upstream fragment.
- `userland-prune` removes only manifest-disabled component ELF outputs, and
  leaves arbitrary files and other components intact.

## Parser boundaries

`reliefos-components` is a bounded, explicit constrained TOML parser, not a shell
text extractor. It accepts the current schema: bare keys, basic single-line
strings without escapes, booleans, version integer, string arrays, components
array-of-tables and comments. Unsupported syntax/fields fail clearly. Nonempty
`api_requires` is deliberately rejected rather than falsely resolved; the current
manifest contains no such field. Build selection is provided here; image/entry/
SDK/API metadata generation belongs to the rootfs/SDK integration.

## Verification (2026-09-19)

All commands use `PATH=/usr/bin:/bin`. No commits, pushes, QEMU runs or source
renames/touches were performed.

- `sh tests/build/test-components.sh`: pass with GCC and Clang strict warnings.
  Required/disabled selection, dependency closure, cycles, unsafe names and
  truncated strings; old valid output survives a rejected manifest.
- Same suite with Clang ASan/UBSan: pass. Log `/tmp/reliefos-components-test.log`.
- `sh tests/build/test-gears-generator.sh`: GCC, Clang and ASan/UBSan pass;
  exactly one upstream implementation marker required, old output preserved.
- `sh tests/build/test-userland-graph.sh`: pass in disposable fixture. No-op mtime,
  distinct objects for same-named main.c files, source deletion drops prior linked
  content, deleted ELF repair, disabled component prune preserving another app.
- Actual clean output: `O=/tmp/leonos-userland-validation`. Configured all three
  Stardust example BUILD symbols to y in that output's config only.
- `make O=/tmp/leonos-userland-validation app-hello app-desktop app-installer
  app-browser app-doom app-mp3play app-glxgears app-stardusthello app-stardustlayout
  app-stardustshowcase installer-userland
  /tmp/leonos-userland-validation/userland/dynlinkerror.elf -j8`: pass.
  Log `/tmp/leonos-userland-validation.log`.
- All `USERLAND_APPS` plus PortableGL .so/.a and Stardust archive built with a
  temporary aggregate Make target: pass. Log `/tmp/leonos-userland-all.log`.
- Repeated real build emitted no CC/LD/PACK operations.
  Log `/tmp/leonos-userland-noop-final.log` (parent's temporary coordination
  message is the only output).
- `readelf` confirms hello ELF64 x86-64 PIE, DT_NEEDED libmimalloc.so.3,
  libreliefos.so.2 and libc.so; dynlinkerror has no dynamic section.
- Initial CC command-line propagation failure in upstream auth was diagnosed:
  recursive Make replaced configured compound CC with plain host clang. Parent
  fixed adapter propagation. This is not a userland workaround.

GUI/app runtime behavior still requires the parent QEMU acceptance matrix.
- Final all-owned/installer no-op: no compiler/linker actions, log
  `/tmp/leonos-userland-all-noop.log`. 60 application ELFs exist in userland/.
- Deleted actual hello ELF and rebuilt: exactly one LD, no CC, restored bytes
  match saved prior output (`cmp` exit 0). Log `/tmp/leonos-userland-repair.log`.
