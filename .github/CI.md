# ReliefOS automation

```mermaid
flowchart TD
    A[PR / main push / manual run] --> B[Select checks]
    A --> C[Workflow and orchestration checks]
    B --> D[Documentation: Sphinx 5 / 7 / 9]
    B --> L[Restore verified downloads and compiler cache]
    L --> E[Build images and Pages once]
    E --> F[Product and core regressions]
    B --> R[Isolated regressions: kernel / auth / pages]
    F --> G[Record commit and SHA-256 / upload products]
    G --> H[BIOS: disk / live / installer]
    G --> I[UEFI: disk / live / installer]
    B --> J[CI result]
    C --> J
    D --> J
    R --> J
    H --> J
    I --> J
    J --> K[Deploy the verified Pages artifact on main]
```

`ci.yml` owns validation and Pages deployment. `check-docs.yml` is a reusable
workflow; it no longer runs separately on branch pushes and PR updates.
SourceHut mirroring and manual code statistics remain independent maintenance
workflows. Job dependencies form the native graph on the Actions run page.

## Selection and the merge check

PRs changing only manuals run documentation checks without compiling images.
README, PR template and maintenance workflow changes still run workflow checks.
Unknown source paths require a full build. Shared build/version/kernel inputs
also run the documentation compatibility matrix. Main pushes and manual runs
always run all stages. Select **CI result** as the required branch check: it
fails for failed, cancelled, missing or unexpectedly skipped required jobs.

`tools/ci.py changes` uses the actual PR base and checked-out merge commit,
including removed and renamed files. It does not depend on the Actions paths
filter's file-count limit. Manual runs default to validation only. Checking
`deploy_pages` explicitly enables publication and is accepted only on `main`.

## Products, signatures and deployment

The build job runs `make all pages` and then
`make RELIEFOS_CI_TEST_GROUP=core test` in one working tree. This lets
product-dependent regressions consume the current SDK, libraries and staging
tree without transferring the entire compiler output between jobs.
Common runner dependencies live in `scripts/ci/install.sh` and the local setup
action. The build checkout includes full Git history for kernel rollback
regressions. Host development packages include FFmpeg, Cairo, GLib and Xvfb.

Three focused regression jobs start alongside the product build, workflow
checks and documentation matrix. `test-ci-kernel` covers concurrent and
incremental builds, the kernel adapter, header boundaries, reproducibility and
submodule rollback; `test-ci-auth` covers authentication staging;
`test-ci-pages` covers complete site assembly. Each job has its own checkout
and output tree and builds only its group's prerequisites. The final gate
requires the product build, every regression group and both boot jobs.
Focused groups install the compiler, native tools and their specific test
dependencies; they do not install the product build's FFmpeg and GUI stack.
The kernel group also installs image and boot tools because its concurrency
contract checks that the complete `make doctor` can run while a build holds
the output lock.

`tools/ci_test_shards.py` owns the test partition and supplies the workflow
matrix. It checks that each focused suite exists and belongs to exactly one
group; new suites join core automatically. Ordinary `make test` continues to
run every suite. `make -j"$(nproc)"` passes the jobserver through the contract
runner to nested Make processes. Suites within a group remain sequential
because some temporarily probe shared source files. Each suite logs its
elapsed seconds, and a dry run never executes its scripts.

The download cache covers both repositories' `cache/downloads`, keyed by both
dependency locks and runner platform. A fallback can supply unchanged archives
after a lock update, but `make fetch` still verifies their hashes and replaces
stale bytes before a build. Verified downloads are saved before compilation,
so a later test failure does not throw them away. ReliefNT fetch is already
delegated by the parent and is not repeated.

`ccache` wraps the standard compiler names for the build and its nested tests.
Compiler caches have separate keys for the product build (4G), kernel tests
(2G), authentication tests (1G) and Pages tests (1G). They check compiler
contents and retain the default header, flag and working-directory checks.
Path rewriting stays disabled so Kbuild source/dependency records retain their
native paths. Each run saves a new cache, even after a test failure, and reports
hits/misses in the run summary. A main-only maintenance job retains at most two
snapshots per role/platform/ref and applies an 8 GiB budget to the new compiler
cache namespace, always preserving the latest main snapshot for each role and
platform. It leaves download, legacy and unrelated caches alone. Only this
maintenance job receives Actions write permission; PR checks do not. No output
tree, signed APK, image or private key is restored from a build cache. GitHub's
cache ref scope separates PR caches from the default branch. The pinned
actionlint binary has a separate platform/version cache; warm runs avoid
installing Go just to rebuild the linter.

Publishing builds use `RELIEFOS_APK_SIGNING_KEY_B64`, with the existing
legacy secret fallback. PRs and validation-only manual runs use
an ephemeral RSA key. APKs, images, the installer download and RPR share one
build and signing cohort. The private key stays in the runner's temporary
directory and is removed before artifact upload, including on failure.

`ci/manifest.json` records the source commit, kernel release, sizes and SHA-256
of all three boot media, `.config` and `kernel-install/install.bundle.json`.
The exact same installer must appear in Pages. Image transfer uses ZIP
compression level 1; the manifest continues to identify the uncompressed
image bytes. The public Pages tree contains
the manifest as `build-manifest.json`. Boot jobs verify all five files both
before and after running; they call the existing acceptance runner directly
and never invoke Make or rebuild an image.

BIOS and UEFI each test disk, live ISO and installer ISO. The runner requires
kernel/PID 1 evidence and the expected graphical login or installer screen,
checks the recorded kernel release, and uses private scratch disks. KVM is
used when accessible, otherwise TCG. Boot logs, screenshots and result records
are retained for seven days; images and Pages artifacts for three days.

Only a successful `CI result` allows the deployment job to consume this run's
Pages artifact. Pages permissions and the `github-pages` environment belong
only to that job. A running main build is allowed to finish; GitHub keeps the
newest pending run in the concurrency group. Newer PR updates cancel
superseded PR runs. GitHub Release publication remains a separate procedure.

## Local validation

```sh
actionlint
shellcheck scripts/ci/*.sh
sh tests/build/test-ci.sh
python3 tools/ci_test_shards.py --check
```

The workflow pins third-party action commits and actionlint's version. Full
product verification uses the existing `make test` and dual-firmware boot
acceptance, rather than treating YAML validation as runtime evidence.
