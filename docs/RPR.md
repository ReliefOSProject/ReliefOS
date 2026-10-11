# ReliefOS Remote Package Repository

ReliefOS RPR is a static, HTTPS-only repository published as a directory of the
GitHub Pages site. Its machine interface has two independent channels, both
relative to the RPR base URL (by default
`https://reliefosproject.github.io/ReliefOS/rpr`):

- `<base>/apk` contains signed ReliefOS APK packages, including the optional
  official applications, the signed APK v3 index, and the corresponding public
  key.
- `<base>/kernel` contains the latest `kernel.sys` plus its version and
  SHA-256 metadata.

The Pages site places this machine interface under `/rpr/` and adds
human-readable HTML pages beside it (package list, per-package pages, kernel
page). The same base URL is used by clients, so it must end in `/rpr`. The
configured default is `https://reliefosproject.github.io/ReliefOS/rpr`; it can be changed
under **Build > ReliefOS remote package repository URL** in `menuconfig`. The
selected value is installed as `/etc/reliefos/rpr.conf`. The canonical command
links (`reliefos-rpr-ping`, `reliefos-rpr-apkcheck`, `reliefos-kernel-update`) append
`/health.txt`, `/apk/...` and `/kernel/...` to that base, so the base URL must
point at the directory that contains `apk/packages.adb`.

## One-time signing-key provisioning

The APK identity must be generated with Alpine `abuild-keygen`. Run the helper
from an Alpine host with `alpine-sdk`, OpenSSL, and an authenticated GitHub CLI:

```sh
./tools/provision_rpr_signing_key.sh ReliefOSProject/ReliefOS
```

The helper runs `abuild-keygen` in a private temporary home, stores the private
key as the base64-encoded GitHub Actions secret
`RELIEFOS_APK_SIGNING_KEY_B64`, prints the public-key SHA-256 fingerprint, and
removes the temporary keypair. It does not write either key into this checkout.
The workflow derives the public key and publishes `/apk/reliefos-rpr.rsa.pub`.
Only public keys are part of the Pages artifact.

After provisioning, the private key must exist only in the GitHub Secret.
Losing the Secret requires a signing-key rotation. A rotation replaces the
Secret and the next `reliefos-rpr-apkcheck` run replaces the client's fixed-name
public key.

## Publishing

Enable GitHub Pages with **Source: GitHub Actions**, then run the **Build ReliefOS Pages**
workflow (`.github/workflows/build-pages.yml`). It triggers on pushes to `main`
and on `workflow_dispatch`. The workflow has no separate RPR job: RPR, the
download page, the home page and the generated Documentation section are built
and deployed together as one atomic release (see `docs/BUILDSYSTEM.md`). It:

1. checks out the complete source tree and installs the normal ReliefOS toolchain;
2. decodes the signing key only under `$RUNNER_TEMP` with mode `0600`;
3. builds the whole release with `make pages`, which depends on the installer
   ISO and on `make rpr-pages`, so kernel, base APK repository, optional
   application APKs and every page come from one build; the Documentation
   section is rendered from the repository `docs/` tree by `tools/build/md2html.awk`
   (pure awk: no Python/Node in the production build path);
4. merges both APK inputs and signs `apk/packages.adb` from only
   `reliefos-*.apk` packages;
5. assembles `kernel/` from artifacts produced in the same build;
6. runs `tools/build/verify-pages.sh`, which rejects a Pages tree containing any
   JavaScript, any PEM private-key marker, a broken link, a raw Markdown leak
   under `docs/`, or a checksum that disagrees with the file it describes;
7. removes the temporary key and deploys with GitHub's OIDC Pages action.

`make rpr-pages` on its own still generates only the RPR subtree under
`out/x86_64/release/rpr-pages/` for local use; `make pages` embeds that subtree
at `pages/rpr/` and additionally emits the Documentation section at
`pages/docs/` in the full site.

The default images contain neither these optional applications nor their APK
files. They are available only from RPR as `reliefos-helloworld`, `reliefos-doom`,
and `reliefos-oschinpt`. `reliefos-doom` includes the launcher, engine, Freedoom
IWAD, and license notice. The legacy `.api` installer remains available for
third-party packages, but ReliefOS no longer publishes these three applications
as `.api` files.

The published tree, relative to the RPR base URL, is:

```text
/rpr/
├── index.html          (human-readable landing page)
├── css/reliefos.css
├── health.txt
├── manifest.json
├── apk/
│   ├── index.html
│   ├── leonos-rpr.rsa.pub
│   ├── packages.adb
│   ├── reliefos-*.apk
│   ├── repository.json
│   └── SHA256SUMS
├── packages/
│   ├── index.html      (generated package list)
│   └── reliefos-<name>.html  (one page per published package)
└── kernel/
    ├── index.html
    ├── kernel.sys
    ├── release.txt
    ├── release.json
    └── SHA256SUMS
```

The Pages home page and download page sit at the site root (`/index.html` and
`/download/`), outside `/rpr/`. Every link inside the RPR tree is relative, so
the base path never has to be baked into the generated HTML.

`release.txt` is the strict line-oriented client protocol. Its
`image_version` and `version` are both `major.minor.patch`; no build counter or
numeric build suffix is published. `format_version` is `3` and the record lists
`kernel_file`/`kernel_sha256`: `kernel.sys` is the single release unit.

Format 1 advertised a paired `middlelayer.sys` and format 2 paired `kernel.sys`
with `loader.elf`; both are boot payloads this system no longer loads, so
`reliefos-kernel-update` refuses those manifests instead of installing a half
update; the legacy `leonos-kernel-update` path remains available. A client
that predates format 3 refuses the format-3 manifest for the same reason, which
means an installed system first has to receive the newer `reliefos-kernel-update`
through `apk upgrade` before it can take a kernel release; that ordering is
deliberate, because a refused manifest leaves the working payload untouched.
The JSON file is informational and intended for external tools.

## Client commands

All network access uses the ReliefOS TLS client and rejects non-HTTPS repository
URLs.

- `reliefos-rpr-ping` checks `/health.txt`. It does not need root; the old
  `leonos-rpr-ping` command remains an alias.
- `reliefos-rpr-apkcheck` requires root. It downloads the public key into
  `/etc/apk/keys`, adds `ndx BASE_URL/apk/packages.adb` to
  `/etc/apk/repositories`, and runs `apk update`. It never disables APK
  signature verification.
- `reliefos-kernel-update --check` strips the build suffix from `uname -r` and
  compares only the three-part `major.minor.patch` image version. A newer build
  of the same image version does not trigger an update. Check mode does not
  modify the system.
- `reliefos-kernel-update` requires root. It downloads `kernel.sys`,
  verifies its SHA-256, then swaps `/boot/reliefos/kernel.sys` inside a single
  commit window. A failed commit restores the previous kernel; only a completed
  commit deletes the backup. Reboot to
  activate the release. Use
  `reliefos-kernel-update -f` (or `--force`) to repeat the replacement even when
  the local and remote image versions match; the same format, checksum and
  rollback protections still apply.
- `reliefos-check-update` requires root because it refreshes APK indexes. It only
  checks the kernel release and installed `reliefos-*` APK packages; it
  does not install updates. APK availability is determined by `apk` itself.

Initial setup on an installed system is:

```sh
reliefos-rpr-ping
sudo reliefos-rpr-apkcheck
sudo reliefos-check-update
sudo apk add reliefos-helloworld reliefos-doom reliefos-oschinpt
```

Run `sudo reliefos-kernel-update` and/or `sudo apk upgrade` only after reviewing
the reported updates.
