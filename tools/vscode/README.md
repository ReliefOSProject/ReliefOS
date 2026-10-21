# VS Code development support

The editor database is exported from the real Kbuild `.cmd` records after a
build. It contains the exact compiler arguments and native working directory
for each built source, including separate application contexts. No independent
set of guessed build flags is maintained.

The database is `out/vscode/compile_commands.json`. Build `out/x86_64/release`
first, or select another completed tree:

```sh
python3 tools/vscode/generate_compile_commands.py --build-dir out/my-build
```

Run `ReliefOS: Generate compile database (all regions)` after building the selected
output tree.  Region-specific generation and clang-tidy
tasks are available from the command palette.  The tasks execute in WSL using
the project path from `leonos.wslProjectRoot`.

For correct `/usr/bin/clang` and generated-header resolution, open the folder
with **Remote - WSL**.  The extension is recommended by this workspace.  Tasks
can also be started from a Windows VS Code window; in that case they still
build in WSL, but IntelliSense should use a Remote-WSL window.

`clang-tidy` must be installed inside the active WSL distribution.  The helper
also accepts `--checks`, `--fix`, and `--warnings-as-errors` when invoked from a
terminal, for example:

```sh
python3 tools/vscode/run_clang_tidy.py --region kernel --warnings-as-errors
```

The generator intentionally excludes upstream third-party source trees from
the first-party database; their headers remain available to IntelliSense.

The only absolute path kept in workspace settings is
`leonos.wslProjectRoot`, because `wsl.exe --cd` requires a Linux path and does
not translate `.` from the Windows task working directory.  All repository
paths and debugger source mapping derive from the workspace or that single
setting.
