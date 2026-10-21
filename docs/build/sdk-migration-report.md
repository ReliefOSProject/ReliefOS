# SDK Make migration report

> 历史验证记录：以下路径、命令和结果对应记录时的构建链。当前 Kbuild 接口与验收见 [Kbuild 迁移记录](kbuild-migration.md)。

## Scope

The Make production graph now builds both the relocatable musl SDK and the
complete `LeonOS4-Developer-SDK.zip` without invoking Python, Ninja, or Meson.
The developer archive is assembled from the new musl/runtime/PAM/auth outputs,
selected new-chain userland libraries, generated component metadata, and the
hand-written `devtools` documentation, examples, templates, and linker files.

The delivered compiler wrapper is the C program
`tools/host/sdk/leonos-musl-cc.c`.  It finds its SDK relative to its own binary,
passes user arguments directly through `execvp`, preserves the LeonOS startup
objects/link order, and resets `-x` before appending binary link inputs.

## Recovery and publication

Both SDK stages publish through temporary directories.  Make names required
headers, startup objects, libraries, and the compiler wrapper as grouped
outputs.  Deleting `include/stdio.h` or `lib/crt1.o` therefore rebuilds the
stage even when its archive is newer.  The full developer ZIP is written to a
temporary path and renamed only after the staged tree passes completeness
checks.

## Compatibility payload

The full archive retains the SDK Makefile, documentation, examples, public
LeonOS/UAPI/musl headers, startup objects, compiler-rt builtins, libc, mimalloc,
`libleonos`, zlib, libpng, PAM, libxcrypt, pkg-config files, licenses, component
metadata, and enabled ncurses, libmagic, Lua, SQLite, PortableGL, and StardustUI
development files.  Optional libraries follow `LEONOS_COMPONENTS_ENABLED`.

## Verification (isolated output)

Output directory: `build/sdk-agent`

- `tests/build/test-sdk-driver.sh`: argv integrity, compile-only/static/dynamic
  modes, relocation, compiler failure propagation, and resource probe failure.
- `tests/build/test-sdk-stage.sh`: fixture builds for both stages, deletion and
  restoration of `stdio.h` and `crt1.o`, docs/examples preservation, and ZIP
  membership.
- Real developer archive build:
  `make -j4 O=build/sdk-agent TARGET_RUSTC=/home/xiaobai/.cargo/bin/rustc build/sdk-agent/packages/LeonOS4-Developer-SDK.zip`.
- Real compiler smoke: the extracted SDK driver compiled
  `examples/helloworld/main.c`; `file` and `readelf` identified an x86-64 PIE
  using `/lib/ld-musl-x86_64.so.1`, and a second `-static` build produced an
  x86-64 static executable without an interpreter segment.
- Real recovery: all four staged copies of `stdio.h` and `crt1.o` were removed,
  rebuilt, and matched their original SHA-256 values.
- `unzip -t` reported no errors.
- Final archive SHA-256 values from this isolated run:
  `bc71379313d20344fbca97923662f40b0ac00ad328203d3553471310b4f22aaf`
  (`leonos-musl-sdk.tar.gz`) and
  `d2693080a9b8c699e8dfd64f90b2f90bc02afb0f0910f14fdec67c951557e778`
  (`LeonOS4-Developer-SDK.zip`).

The top-level Makefile still has a later-phase failure recipe attached to
`sdk`.  Integration must remove `sdk` from that stub target list; `mk/sdk.mk`
already supplies the real `sdk` prerequisites and target.
