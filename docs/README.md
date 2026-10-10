# ReliefOS Documentation

This directory tracks the architecture and operational notes that are useful
when changing the kernel, loader, installer, or user ABI.

- [ABI](ABI.md): syscall subset and device model.
- [Kernel / user-space boundaries](KERNEL_USERSPACE_BOUNDARIES.md): what the
  kernel and the storage layer own after the middle layer was removed, and how
  old `LEONACL.SYS`, handoff and RPR formats are treated.
- [Linux ABI audit](LINUX_ABI_AUDIT_2026-09-07.md) and
  [per-syscall status](LINUX_ABI_SYSCALLS_2026-09-07.csv): remaining compatibility work.
- [musl migration](MUSL_MIGRATION_2026-09-08.md): runtime, SDK and binary compatibility.
- [Unix permissions](POSIX_PERMISSIONS_2026-09-08.md): implemented contracts and verification.
- [Timer validation](LINUX_ABI_TIMER_2026-09-09.md): timer and Vim evidence and remaining gaps.
- [Syscalls](SYSCALLS.md): syscall entry convention, syscall table,
  `mmap`/`munmap`, process calls, and ioctl groups.
- [Filesystem](FILESYSTEM.md): Unix-style root paths, ext4 default root plus legacy ext2, exFAT/FAT32/ISO 9660 support,
  bundled `.hlp` help documents, and current limits.
- [Drivers](DRIVERS.md): bootstrap drivers, built-in device drivers, ABI, and
  management policy.
- [Build system](BUILDSYSTEM.md): GNU Make layout, target graph (kernel /
  userland / runtime / sdk / installer / rpr-pages / pages), incremental and
  concurrent build rules, and Pages assembly contract.
- [APK preparation](APK_PREPARATION.md): rootfs-to-APK packaging and the
  signing pipeline that feeds `make rpr-pages`.
- [Remote Package Repository](RPR.md): RPR machine interface, `make pages`
  assembly, GitHub Pages publishing, and how `verify-pages.sh` guards the tree.
- [EEVDF scheduler](EEVDF_SCHEDULER.md): fair-scheduling rules, per-CPU
  fairness, SMP state, and known limitations.
- [Execution lock](EXECUTION_LOCK.md): the global kernel service transaction,
  the 2026-09-18 read-side anonymous-fault fast path, and the boundaries that
  still serialise cross-core syscalls.
- [Desktop performance](DESKTOP_PERFORMANCE.md): compositor refresh path and
  the framebuffer-present behaviour that gates Doom and Terminal frame rates.
- [Kernel Debug](KERNEL_DEBUG.md): five-click activation, one-shot boot state,
  `kerneldebug.sys`, and the ostui diagnostic interface.
- [Boot and Integrity](BOOT_AND_INTEGRITY.md): loader boot flow, SHA-256
  component checks, installer compatibility, and trust boundary.
- [Build and Installer](BUILD_AND_INSTALLER.md): generated build graph,
  packaging outputs, installer payload layout, and WSL validation commands.
- [Application Registry](APP_REGISTRY.md): manifest format, discovery roots,
  compatibility behavior, and SDK API.
- [Advanced Installation](ADVANCED_INSTALL.md): TTY-only manual installation,
  GPT initialization, partitioning, filesystem setup, and UEFI payload install.
- [Browser](BROWSER.md): `browser.elf`, current HTML renderer limits, and the
  staged path toward a full litehtml port.
- [Third-Party Code](THIRD_PARTY.md): submodule paths, upstream licenses, and
  integration status for bundled external code.

## Tools

`tools/analyze_boot_log.py` parses loader, kernel, ELF, dynamic
linker, CPU exception, storage, and ACPI messages. It accepts a file or
standard input and preserves one-based evidence line numbers:

```sh
python tools/analyze_boot_log.py boot.log
python tools/analyze_boot_log.py boot.log --json tools/dist/boot-report.json
cat boot.log | python tools/analyze_boot_log.py - --strict
python tools/analyze_boot_log.py --self-test
```

`--strict` returns a non-zero status when an error or fatal finding is present,
which makes the analyzer suitable for CI/QEMU smoke-test wrappers.

`tools/check_licenses.py` checks initialized submodules, staged image and
installer payloads, SDK directories or ZIP archives, and the installer
acknowledgements policy.  It uses an explicit license manifest so disabled
components are not reported as missing:

```sh
python tools/check_licenses.py
python tools/check_licenses.py --strict
python tools/check_licenses.py --sdk reliefos-musl-sdk.tar.gz \
  --json tools/dist/license-report.json
python tools/check_licenses.py --self-test
```

Raw FAT/VMDK/ISO files are reported as skipped; pass the mounted or staged
directory (or a ZIP) to verify their contents.  The default excluded-credit
policy rejects `llama2.c`, `TinyLlama`, and `karpathy` in the installer's
acknowledgements page.  Add project-specific forbidden names with repeated
`--excluded-credit` options.
