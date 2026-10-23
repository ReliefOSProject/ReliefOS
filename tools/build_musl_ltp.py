#!/usr/bin/env python3
"""Build unmodified, pinned LTP tests with musl and Linux v6.12 headers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
LTP_REVISION = "2279d708c817db657611a30c427c7d06c4360049"
SOURCES = (
    ("ltp-20260930.tar.gz", f"https://codeload.github.com/linux-test-project/ltp/tar.gz/{LTP_REVISION}",
     "35146cb1d0e82517aac8dbefbd88bd96300ce383736aae069e435fd991ea50bb", f"ltp-{LTP_REVISION}"),
    ("linux-6.12.tar.xz", "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz",
     "b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb", "linux-6.12"),
)
TESTS = {"getcwd01": "getcwd", "fcntl01": "fcntl", "fstat02": "fstat", "mprotect01": "mprotect",
         "chmod01": "chmod", "fchmod01": "fchmod", "chown01": "chown"}
POSIX_TESTS = ("pthread_create", "pthread_join", "pthread_mutex_lock", "pthread_cond_wait",
               "pthread_cancel", "pthread_key_create", "pthread_barrier_wait",
               "pthread_rwlock_rdlock", "pthread_once", "pthread_mutex_timedlock",
               "pthread_cond_timedwait", "pthread_mutex_trylock", "sem_timedwait", "pthread_spin_lock")


def run(command, **kwargs):
    subprocess.run([str(arg) for arg in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/musl/sdk")
    parser.add_argument("--cache", type=Path, default=ROOT / "build")
    parser.add_argument("--out", type=Path, default=ROOT / "build/musl/ltp")
    parser.add_argument("--proxy", help="override the proxy from the environment (optional)")
    args = parser.parse_args()
    cache, output, sdk = args.cache.resolve(), args.out.resolve(), args.sdk.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    for filename, url, digest, directory in SOURCES:
        archive = cache / filename
        if not archive.is_file():
            run(["curl", "--fail", "--location",
                 *(["--proxy", args.proxy] if args.proxy is not None else []), url, "-o", archive])
        with archive.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != digest:
            raise SystemExit(f"Source checksum mismatch: {archive}")
        if not (cache / directory).is_dir():
            with tarfile.open(archive) as source:
                source.extractall(cache, filter="data")
    linux = cache / "linux-6.12"
    headers = cache / "linux-6.12-headers"
    ltp = cache / f"ltp-{LTP_REVISION}"
    run(["make", "-C", linux, "ARCH=x86_64", "headers_install", f"INSTALL_HDR_PATH={headers}"])
    run(["make", "autotools"], cwd=ltp)
    env = {**os.environ, "CC": str(sdk / "bin/reliefos-musl-cc"),
           "CFLAGS": f"-O2 -I{headers / 'include'}", "LDFLAGS": "-static",
           "AR": "llvm-ar", "RANLIB": "llvm-ranlib", "PKG_CONFIG_LIBDIR": "/nonexistent"}
    run([ltp / "configure", "--host=x86_64-linux-musl", "--build=x86_64-pc-linux-gnu",
         "--without-numa", "--without-libcap", "--without-openssl", "--without-tirpc"], cwd=ltp, env=env)
    run(["make", "-j8", "include-all", "lib-all"], cwd=ltp)
    for test, directory in TESTS.items():
        location = ltp / "testcases/kernel/syscalls" / directory
        run(["make", "-C", location, test])
        shutil.copy2(location / test, output / f"{test}.elf")
    posix = ltp / "testcases/open_posix_testsuite"
    for test in POSIX_TESTS:
        run([sdk / "bin/reliefos-musl-cc", "-O2", "-static", "-pthread",
             f"-I{headers / 'include'}", f"-I{posix / 'include'}",
             posix / f"conformance/interfaces/{test}/1-1.c", posix / "lib/common.c",
             "-o", output / f"{test}_1-1.elf"])
    run([sdk / "bin/reliefos-musl-cc", "-O2", "-static", ROOT / "tools/tests/ltp_guest_runner.c",
         "-o", output / "ltp-runner.elf"])
    shutil.copy2(ltp / "COPYING", output / "COPYING.ltp")
    (output / "manifest.json").write_text(json.dumps({
        "ltp_revision": LTP_REVISION, "linux_headers": "6.12", "libc": "musl",
        "linkage": "static with mimalloc", "tests": list(TESTS),
        "open_posix_tests": [f"{test}/1-1" for test in POSIX_TESTS],
        "sources": [{"url": url, "sha256": digest} for _, url, digest, _ in SOURCES],
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
