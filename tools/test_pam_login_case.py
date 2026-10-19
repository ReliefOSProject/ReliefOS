#!/usr/bin/env python3
"""Exercise production GUI PAM login with real musl PAM DSOs in an isolated host namespace."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def has_staged_pam_tree(dependencies: Path) -> bool:
    stage = dependencies / "root"
    return all((stage / path).is_dir() for path in
               ("usr/include", "lib/security", "sbin"))


def default_dependencies() -> Path:
    build_dependencies = ROOT / "build/auth-upstream"
    if ((build_dependencies / "linux-pam-build.json").is_file() and
            has_staged_pam_tree(build_dependencies)):
        return build_dependencies
    return ROOT / "out/xorg-vmware-debug/pam"


def main():
    override = os.environ.get("RELIEFOS_AUTH_UPSTREAM_ROOT")
    dependencies = Path(override).resolve() if override is not None else default_dependencies().resolve()
    work = Path(os.environ.get("RELIEFOS_TEST_OUT", ROOT / "build/auth-upstream")).resolve()
    work.mkdir(parents=True, exist_ok=True)
    stage = dependencies / "root"
    recipe_path = dependencies / "linux-pam-build.json"
    default_sdk = ROOT / "out/xorg-vmware-debug/sdk/reliefos-musl-sdk"
    sdk = Path(os.environ.get("RELIEFOS_MUSL_SDK", default_sdk)).resolve()
    if recipe_path.is_file():
        compiler = json.loads(recipe_path.read_text())["compiler"]
    else:
        compiler = [os.environ.get("RELIEFOS_MUSL_CC", str(sdk / "bin/reliefos-musl-cc"))]
    default_auth_root = ROOT / "out/xorg-vmware-debug/auth/root"
    auth_root = Path(os.environ.get("RELIEFOS_AUTH_ROOT", default_auth_root)).resolve()
    binary = work / "pam-login-case.elf"
    subprocess.run([*compiler, "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-U_GNU_SOURCE", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections,--export-dynamic",
                    "-Ikernel/reliefnt/include", "-Iinclude", "-Ikernel/reliefnt/include/uapi", "-idirafter", "userland/runtime/include",
                    "-I" + str(stage / "usr/include"), "tools/tests/pam_login_case_test.c",
                    "userland/auth/standard_accounts.c", "userland/auth/account_store.c",
                    "userland/runtime/src/auth_password.c", "-L" + str(stage / "lib"),
                    "-L" + str(auth_root / "lib"),
                    "-lpam", "-lcrypt", "-pthread", "-o", str(binary)], cwd=ROOT, check=True)
    command = ["bwrap", "--unshare-user", "--uid", "0", "--gid", "0", "--unshare-pid",
               "--unshare-net", "--die-with-parent", "--ro-bind", "/", "/", "--proc", "/proc",
               "--dev", "/dev", "--tmpfs", "/tmp", "--dir", "/tmp/lib",
               "--tmpfs", "/etc", "--tmpfs", "/run",
               "--ro-bind", str(stage / "lib/security"), "/lib/security",
               "--ro-bind", str(ROOT / "system/test-accounts/group"), "/etc/group"]
    default_musl_sysroot = ROOT / "build/musl/sysroot"
    if not default_musl_sysroot.is_dir():
        default_musl_sysroot = ROOT / "out/xorg-vmware-debug/sysroot/musl"
    musl_sysroot = Path(os.environ.get("RELIEFOS_MUSL_SYSROOT", default_musl_sysroot)).resolve()
    loader = musl_sysroot / "lib/libc.so"
    loader_target = str(Path("/lib/ld-musl-x86_64.so.1").resolve())
    command += ["--ro-bind", str(loader.resolve()), loader_target]
    # pam_unix launches unix_chkpwd with an intentionally empty environment.
    # Make its linked musl DSOs available through the loader's normal /lib
    # search path instead of relying on LD_LIBRARY_PATH, which is only set on
    # the test process and is not inherited by that privileged helper.
    helper_dsos = (
        (stage / "lib/libpam.so.0.85.1", "libpam.so.0"),
        (auth_root / "lib/libcrypt.so.2.0.0", "libcrypt.so.2"),
        (musl_sysroot / "lib/libc.so", "libc.so"),
    )
    for library, soname in helper_dsos:
        if not library.is_file():
            raise FileNotFoundError(f"PAM helper runtime is missing {library}")
        target = str((Path("/lib") / soname).resolve())
        command += ["--ro-bind", str(library.resolve()), target]
    bound_libraries = set()
    for library_root in (stage / "lib", auth_root / "lib", sdk / "lib"):
        for library in library_root.glob("*.so*"):
            target = "/tmp/lib/" + library.name
            if library.is_file() and library.name not in bound_libraries:
                command += ["--ro-bind", str(library.resolve()), target]
                bound_libraries.add(library.name)
    for helper in (stage / "sbin").iterdir():
        if helper.is_file():
            command += ["--ro-bind", str(helper.resolve()), str((Path("/sbin") / helper.name).resolve())]
    command += ["--setenv", "LD_LIBRARY_PATH", "/tmp/lib"]
    subprocess.run([*command, str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
