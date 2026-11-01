#!/usr/bin/env python3
"""Exercise BusyBox-to-GNU path ownership transfer using real signed APKs.

Creates an isolated legacy BusyBox fixture, then installs the actual current
coreutils and BusyBox packages without force-overwrite or unsigned-package flags.
Only the disposable test root is modified; package scripts run in a user namespace.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.output.resolve()
    if output.exists() or not output.is_relative_to((ROOT / "out").resolve()):
        parser.error("output must be a new directory under out/")
    output.mkdir(parents=True)
    apk = build / "upstream/apk/apk.static"
    repository = build / "packages/apk/repository"
    root, payload = output / "root", output / "legacy-busybox"
    (payload / "bin").mkdir(parents=True)
    keys = root / "etc/apk/keys"
    shutil.copytree(build / "rootfs/managed/etc/apk/keys", keys)
    env = {**os.environ, "TMPDIR": str(ROOT / "out/tmp")}
    with (output / "transaction.log").open("w") as log:
        def run(command, **kwargs):
            process = subprocess.run([str(part) for part in command], cwd=ROOT, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
            log.write(process.stdout.decode(errors="replace"))
            log.flush()
            process.check_returncode()
            return process.stdout.decode(errors="replace")

        def package(*command):
            return run(["unshare", "-Ur", apk, *command])

        private = output / "fixture-key.pem"
        run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", private])
        private.chmod(0o600)
        run(["openssl", "pkey", "-in", private, "-pubout", "-out", keys / "coreutils-fixture.rsa.pub"])
        shutil.copy2(build / "userland/busybox.elf", payload / "bin/busybox")
        programs = set((ROOT / "userland/coreutils/programs.list").read_text().splitlines())
        old_paths = []
        for absolute in (build / "userland/busybox.links").read_text().splitlines():
            path = Path(absolute)
            # dd and yes were already GNU; test the paths actually handed over
            # by this change plus the shell required by real APK triggers.
            if path.name in programs - {"dd", "yes"} or path.name in {"sh", "ash"}:
                destination = payload / absolute.lstrip("/")
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.symlink_to(os.path.relpath("/bin/busybox", str(path.parent)))
                if path.name in programs:
                    old_paths.append(absolute)
        legacy = output / "reliefos-busybox-1.0-r0.apk"
        package("mkpkg", "--files", payload, "--output", legacy, "--sign-key", private,
                "--info", "name:reliefos-busybox", "--info", "version:1.0-r0", "--info", "arch:x86_64",
                "--info", "description:Private legacy ownership fixture", "--info", "license:GPL-2.0-only",
                "--info", "provides:/bin/sh")
        common = ["--root", root, "--arch", "x86_64", "--repositories-file", "/dev/null",
                  "--repository", repository / "packages.adb"]
        musl = list(repository.glob("reliefos-musl-[0-9]*.apk"))
        if len(musl) != 1:
            raise RuntimeError("expected exactly one current musl package")
        package(*common, "--initdb", "add", legacy, musl[0])
        previous = package(*common, "info", "--who-owns", *old_paths)
        if len(previous.splitlines()) != len(old_paths) or any(
                " is owned by reliefos-busybox-1.0-r0" not in line for line in previous.splitlines()):
            raise RuntimeError("legacy fixture did not own every transferred command")
        package(*common, "add", "--upgrade", "reliefos-busybox", "reliefos-coreutils")
        paths = sorted(set(old_paths) | {"/usr/bin/" + name for name in programs})
        owners = package(*common, "info", "--who-owns", *paths)
        if len(owners.splitlines()) != len(paths) or any(
                " is owned by reliefos-coreutils-" not in line for line in owners.splitlines()):
            raise RuntimeError("GNU command paths were not transferred exclusively to coreutils")
        for path in paths:
            if (root / path.lstrip("/")).resolve() != root / "usr/bin/coreutils":
                raise RuntimeError(f"GNU command still resolves to another provider: {path}")
        if (root / "bin/sh").resolve() != root / "bin/busybox":
            raise RuntimeError("BusyBox shell lost its entry during upgrade")
        # A genuine host-kernel chroot executes the installed target ELF. This
        # checks the transaction; native ReliefOS behavior is tested separately.
        run(["unshare", "-Ur", "chroot", root, "/usr/bin/cat", "--version"])
        result = {"success": True, "transferred_legacy_paths": len(old_paths), "gnu_paths": len(paths),
                  "signed_packages": True, "force_overwrite": False,
                  "platform": "host Linux user namespace; APK ownership transaction only"}
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
