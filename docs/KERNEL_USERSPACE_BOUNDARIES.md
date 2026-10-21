# Kernel / user-space boundaries

This page records who owns each boot-time and file-metadata responsibility
after the middle layer (`middlelayer/osmlayer`, `middlelayer.sys`) was removed,
and what happens to data and binaries produced by the old layout. It is the
authority for the question "where does this call go now?"; the boot sequence
itself is in [BOOT_AND_INTEGRITY.md](BOOT_AND_INTEGRITY.md) and the published
user ABI is in [ABI.md](ABI.md).

Before this change the kernel reached a separately loaded Ring-0 module through
one callback table (`struct leonos_middlelayer_api`) and one RPC opcode per
service (`LEONOS_AUTH_OP_*`, `LEONOS_UNICODE_OP_*`, `LEONOS_VFS_OP_*`,
`LEONOS_MOUNT_KIND_*`). Every entry was an indirection over a call that could
have been direct, and several entries had no production caller at all. The
indirection and the dead entries are gone; the live ones moved to the single
owner that already held the data.

## Owners after the migration

| Concern | Owner | Interface |
| --- | --- | --- |
| UTF-8 / UTF-16LE transcoding for on-disk names | `kernel/reliefnt/lib/text_utf16.c` | `text_utf8_to_utf16le()`, `text_utf16le_to_utf8()` |
| POSIX permission and ACL metadata on FAT32/exFAT (`LEONACL.SYS`) | `kernel/reliefnt/fs/security/sidecar.c` | `storage_sidecar_permissions()`, `storage_sidecar_note_deleted()`, `storage_sidecar_note_renamed()` |
| Kernel DAC decision (task credentials against a permission value) | `kernel/reliefnt/fs/security/access.c` | `fs_permissions_check()`, `fs_permissions_parent()`, `fs_permissions_get()` |
| Path and symlink resolution with search permission | `kernel/reliefnt/fs/security/access.c` | `fs_permissions_resolve()`, `fs_permissions_resolve_flags()` |
| Root and installer-root mount choice at boot | `kernel/reliefnt/fs/vfs/mount.c` | `storage_mount_boot_root()` |
| Device inventory shown to userland | `userland/apps/device-agent` over the devmand IPC | `reliefos_device_list()` in `userland/runtime/src/devmand_client.c` |
| Accounts, passwords, sessions | passwd/shadow/group/gshadow plus Linux-PAM | `docs/SUDOERS_PAM_STATUS.md`, `docs/POSIX_PERMISSIONS_2026-09-08.md` |

## Migration table (historical middle-layer removal record)

The old names and source paths below record that earlier removal. The live
owner paths for the renamed tree are listed above.

| Original entry | Production consumers | New owner | New interface | Old-format compatibility |
| --- | --- | --- | --- | --- |
| `osmlayer_unicode_utf8_to_utf16le()` / `osmlayer_unicode_utf16le_to_utf8()` (`LEONOS_UNICODE_OP_UTF8_TO_UTF16LE`, `LEONOS_UNICODE_OP_UTF16LE_TO_UTF8`) | FAT32 LFN build/validate/render, exFAT name decode and name match | `kernel/ntclks/kernel/ntclks/lib/text_utf16.c` | `text_utf8_to_utf16le()`, `text_utf16le_to_utf8()` | Byte-for-byte: same U+FFFD substitution with one-byte advance, same rejection of overlong forms, surrogates and codepoints `>= 0xF5`, same "length needed" reporting when the buffer is short. Existing names on media are unchanged. |
| `LEONOS_UNICODE_OP_LAYOUT_UTF8` | none (no kernel caller; text layout lives in libc) | deleted | `leonos_text_layout_utf8()` in `include/leonos/text.h` | Not applicable; the opcode was unreachable. |
| `osmlayer_auth_op(LEONOS_AUTH_OP_FSPERM)` via `fs_acl_dispatch()` / `fs_acl_fill_actor()` | `fs_permissions_get()`, `fs_permissions_store()` (`kernel/ntclks/fs/permissions.c`), which back `stat`/`chmod`/`chown` and every `fs_permissions_check()` | `kernel/ntclks/drivers/bootstrap/storage/storage_sidecar.c` (metadata) plus `kernel/ntclks/fs/permissions.c` (decision) | `storage_sidecar_permissions(path, value, write)` | `LEONACL.SYS` on-disk format is untouched: magic `LCAL`, version 1 and 2, FNV-1a checksum over `[16,len)`, TLV records, 12-byte ACEs, the legacy deny bit, and the 8192-byte / 64-record caps. Version-1 records are still read and are re-encoded to version 2 on the next write, as before. Corrupt metadata still fails closed (`-EIO`) instead of default-allowing. |
| `osmlayer_auth_op(LEONOS_AUTH_OP_FSPERM)` notify actions (`fs_acl_notify()` for unlink and rename) | `syscall.c` unlink/rmdir/rename paths | `kernel/ntclks/drivers/bootstrap/storage/storage_sidecar.c` | `storage_sidecar_note_deleted()`, `storage_sidecar_note_renamed()` | Same best-effort contract: a failed metadata cleanup does not fail a completed unlink or rename, and only FAT32/exFAT volumes that actually hold a sidecar are touched. |
| `osmlayer_auth_op(LEONOS_AUTH_OP_AUTHORIZE)` via `authz_check_path()` / `authz_check_install()` | `execve`, truncate, unlink, rmdir, rename argument checks | `kernel/ntclks/fs/permissions.c` | `fs_permissions_check()`, `fs_permissions_parent()` | The role-based checks (`LEONOS_AUTH_ROLE_ADMIN`, `TASK_FLAG_ELEVATED_ADMIN`) remain in the kernel task record; behaviour is the same DAC decision the bridge forwarded, minus the round trip. No permission check was removed or relaxed. |
| `LEONOS_VFS_OP_RESOLVE_PATH` (`osmlayer_vfs_resolve_path`) | none: `syscall.c` already resolved paths in the kernel | `kernel/ntclks/fs/permissions.c` (existing) | `fs_permissions_resolve()`, `fs_permissions_resolve_flags()` | Not applicable; the middle-layer copy was a second implementation with no caller. |
| `osmlayer_bridge_mount_policy()` and `struct leonos_mount_policy` (`LEONOS_MOUNT_KIND_*`, `LEONOS_MOUNT_FLAG_*`) | Boot-time root mount only (`kernel_start`) | `kernel/ntclks/drivers/bootstrap/storage/storage_mount.c` | `storage_mount_boot_root(boot, ramdisk_root)` | The installer/live decision now comes from the kernel command line (`mode=installer`, `mode=live`) and the `leonos-installer-root` module tag, both of which already existed. GRUB configurations that passed those arguments boot identically. |
| `device_catalog` op plus `struct leonos_device_catalog_query` / `struct leonos_raw_device_info` | none: the formatting helpers in `syscall.c` had no call sites | `userland/apps/device-agent` (already the live producer) | `leonos_device_list()` | `struct leonos_device_info` and the devmand wire format are unchanged; only the unused kernel-side formatter disappeared. |
| `osmlayer_bridge_syscall()` (`LEONOS_SYSCALL` hook into `gui.rs`, `ipc.rs`, `posix.rs`) | none: no caller existed in `kernel/ntclks/kernel/ntclks/syscall.c` | deleted | Linux ABI syscalls plus the LeonOS syscalls implemented in `kernel/ntclks/kernel/ntclks/syscall.c` | Not applicable. No reachable syscall changed number, argument layout, or return convention. |
| Account store `/var/lib/leonos/accounts.db`, salted SHA-256 checks, session policy | already migrated to passwd/shadow + Linux-PAM before this change; the middle-layer copy was unreachable | deleted with the module | PAM, shadow, sudo | A populated private `accounts.db` is still rejected rather than silently converted, as recorded in `kernel/ntclks/include/uapi/README.md`. |
| Boot self-test `[osmlayer] selftest passed=5/5` | boot log only | deleted with the module | an interim kernel SHA-256 gate (`CONFIG_LOADER_VERIFY_SHA256`) and handoff version check, themselves since removed with the retired intermediate boot stage | Boot logs no longer contain an `[osmlayer]` line. `tools/analyze_boot_log.py` was updated to stop treating its absence as a degradation. |
| `struct leonos_middlelayer_api`, `struct leonos_kernel_services`, `leonos_middlelayer_module_init_fn` | the retired intermediate boot stage, kernel bridge | deleted | direct kernel-internal calls | Removed from `include/leonos/boot_handoff.h` and from both SDK copies; the handoff header has since been deleted entirely. |
| `struct leonos_boot_handoff.middlelayer`, `.middlelayer_api` | the retired intermediate boot stage wrote, `mm.c` reserved, `kernel.c` logged | deleted | the Multiboot2 module ranges | At the time `LEONOS_BOOT_HANDOFF_VERSION` was `7u` and a version-6 handoff was rejected in `boot_handoff_is_current()`. The handoff record and its version gate have since been removed together with the intermediate boot stage: GRUB now loads `kernel.sys` directly and the kernel reads the module ranges from the Multiboot2 information. |
| `struct leonos_system_info.middlelayer_name` | `userland/runtime/src/procsys.c`, `userland/apps/osver` | kept as an offset-preserving gap | `reserved_subsystem_name`, always empty | Binaries built against the old header still read every other field at the same offset and see an empty string. The "Middle layer" row was dropped from the About window instead of being filled with a placeholder name. |
| `LEONOS_PATH_OSMLAYER_MANIFEST`, `LEONOS_PATH_BOOT_MIDDLELAYER` | no production consumer | deleted from `include/leonos/layout.h` and its three mirror copies | n/a | `/usr/lib/leonos/osmlayer.manifest` is no longer staged into any root filesystem. |
| `kernel/ntclks/include/uapi/leonos/permissions.h` (`LEONOS_AUTH_OP_POSIX_PERMISSIONS`, `LEONOS_PERMISSIONS_GET/SET`, `struct leonos_permissions_request`) | kernel ACL bridge only; userland uses `stat`/`chmod`/`chown` | header deleted; `struct leonos_permissions` moved to `kernel/ntclks/kernel/ntclks/include/ntclks/storage.h` | Linux `struct stat`, `chmod`, `chown` | The permission triple is now clearly kernel-internal, so no internal RPC envelope remains in a published header. |
| `LEONOS_AUTH_OP_*`, `LEONOS_AUTHZ_*`, `struct leonos_authz_request` (`include/leonos/auth.h`) | bridge and historical tests only | deleted | `LEONOS_AUTH_ROLE_*` and the `leonos_auth_*` calls, unchanged | Roles are still published; only the RPC opcodes and request struct left the ABI. |
| RPR kernel release `format_version=1` (kernel + middlelayer pair) | `userland/storage/leonos-kernel-update`, `tools/build/rpr-pages.sh` | first `format_version=2` (kernel + retired intermediate stage pair), now `format_version=3` (kernel.sys as the single release unit) | `release.txt` keys `format_version`, `image_version`, `version`, `kernel_file`, `kernel_sha256` | Format-1 and format-2 manifests are refused with an explicit message, because they advertise payloads this system does not load. `kernel.sys` is swapped in one commit window and a failed commit restores the previous kernel. |

## Build and packaging removals

`make middlelayer`, `MIDDLELAYER_SYS`, the middlelayer link step, the
`middlelayer.sys` copies in `tools/build/efi-stage.sh` and
`tools/build/rpr-pages.sh`, `LEONOS_LOADER_MIDDLELAYER_SHA256`,
`middlelayer_name` in `configs/build-version`, and the
`module2 /leonos/middlelayer.sys` lines in `boot/grub/installer.cfg` and
`boot/grub/live.cfg` are all gone. Because Rust was used only by the middle
layer, `TOOLCHAIN_RUSTC`, the `make doctor` Rust probe, `leonos-config
--rustcfg`, and the CI `Install Rust` steps were removed with it; `rustc` is no
longer a build prerequisite.

`tools/gen_loader_integrity.py` was deleted. It duplicated the build's
integrity-hash generator, and leaving two generators for one header
contradicted single ownership; both generators have since been removed
together with the integrity chain they served.

## Status

Implemented against the current tree but **not compiled, not tested, not run in
a virtual machine, and not reviewed**. See the handoff notes in the change
description for the test files that the interface changes require the owning
agent to update.
