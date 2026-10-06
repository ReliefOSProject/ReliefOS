# Code Statistics

`tools/count_code.py` builds a filtered list of project files and passes it to
the selected counter. `cloc` is the default and preserves the historical
report format. `scc` adds complexity, COCOMO, and LOCOMO estimates. The default
configuration is `tools/codecount.json`.

Run it from the repository root:

```sh
python3 tools/count_code.py
python3 tools/count_code.py --languages
python3 tools/count_code.py --format json --output build/code-count.json
python3 tools/count_code.py --format markdown --output build/code-count.md
python3 tools/count_code.py --jobs 8
python3 tools/count_code.py --no-progress --format json
python3 tools/count_code.py --engine scc
python3 tools/count_code.py --engine scc --cocomo-project-type semi-detached \
  --avg-wage 75000 --overhead 2.4 --eaf 1.0 --locomo-preset local
python3 tools/count_code.py --history --history-chart build/code-growth.svg
python3 tools/count_code.py --history --format json --output build/code-history.json
python3 tools/count_code.py --history --format markdown --output build/code-history.md
```

`--format markdown` produces the complete human-readable report: totals,
directory groups, language groups, and when `--engine scc` is used, COCOMO and
LOCOMO estimates. It is appropriate for CI summaries and review artifacts.

## GitHub Actions

The **Code Statistics** workflow is intentionally manual-only. In GitHub,
open **Actions**, select **Code Statistics**, then choose **Run workflow**. It
recursively checks out every Git submodule, runs this tool with the repository
exclusions, and writes the complete Markdown report to the workflow's standard
job summary by writing directly to `$GITHUB_STEP_SUMMARY`. The summary is
rendered directly in the workflow run page and is not dependent on a generated
or uploaded report file.

The standard exclusions cover build products and temporary directories such as
`build/`, `dist/`, `.git/`, `__pycache__/`, `buildsystem/deps/`, and
`buildsystem/tmp/`. Common binary and compiler-generated file suffixes are
also excluded. The repository configuration additionally excludes the optional
`third_party/llama2.c` component, because that tree is not part of ReliefOS's
code-size statistics. These path exclusions are recursive,
so all files below each directory are omitted.

Adjust persistent exclusions in `tools/codecount.json`:

```json
{
  "exclude": ["third_party/example", "system/generated/**"],
  "exclude_dirs": ["fixtures"],
  "exclude_files": ["*_generated.c"],
  "exclude_languages": ["Markdown"],
  "include_languages": []
}
```

Command-line exclusions apply only to the current run:

```sh
python3 tools/count_code.py --exclude userland/apps/doom --exclude-file '*.snap'
python3 tools/count_code.py --exclude-dir test --group-depth 2
```

By default, the tool starts up to four worker threads. The report always keeps
top-level parts such as `userland` and `third_party` separate. Large parts are
internally split into bounded batches, so a large dependency tree or the
userland sources do not monopolize one worker. Use `--shard-threshold` and
`--shard-size` to tune this behavior. Results are merged back into the same
part before the final report. Use `--jobs 1` to force a single worker, or
choose a larger value for a fast machine. Progress is sent to stderr so
JSON/text output on stdout stays machine-readable; use `--no-progress` to
suppress it.

Use `--no-config` to ignore the repository configuration, or
`--no-default-excludes` when a complete diagnostic scan of generated files is
needed. `cloc` must be installed and available in `PATH`; use `--cloc` to
select a specific executable. For `scc`, install the upstream tool and use
`--engine scc` or `--scc PATH`. `scc` receives the same explicit filtered file
list and ignores repository-local ignore files, so `codecount.json` remains the
single source of truth for exclusions.

The `scc` COCOMO values use Basic COCOMO coefficients. `--avg-wage` is an
annual wage, `--overhead` is the non-salary multiplier, and `--eaf` is the
effort adjustment factor. LOCOMO is a rough LLM code-regeneration estimate;
its `large`, `medium`, `small`, and `local` presets describe only pricing and
output throughput, not a promise about any specific model.

Git history growth is available with `--history`. It performs one
`git log --numstat` scan, so it is substantially faster than checking out every
commit and running `cloc` repeatedly. The default follows the first-parent
history and reports cumulative physical text lines after the same path and file
exclusions used by the normal report. `--history-all-branches` includes
non-merge commits from all refs, but its cumulative line series should be read
as aggregate change activity rather than one branch's exact tree size. Use
`--history-chart FILE.svg` to create a dependency-free SVG line chart; JSON and
text output remain available through `--format` and `--output`.

The host-side regression test uses a temporary fixture and does not modify the
repository:

```sh
python3 tools/test_code_count.py
```
