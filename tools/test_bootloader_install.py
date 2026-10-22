"""Host publication and refusal tests for the shared installer API."""
from pathlib import Path
import os
import hashlib
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    """Build production sources with sanitizers and check private target bytes."""
    with tempfile.TemporaryDirectory(prefix="boot-install-") as temporary:
        work = Path(temporary)
        executable = work / "install"
        grub = Path(os.environ.get("GRUB_HOST_ROOT", ROOT / "out/dualboot-build/grub-host")).resolve()
        for tool in ("grub-mkimage", "grub-bios-setup"):
            assert (grub / "bin" / tool).is_file(), "build locked GRUB first: make grub-host"
        adapter = work / "grub-install"
        adapter.write_text("#!/bin/sh\nset -eu\n" + f"grub={shlex.quote(str(grub))}\n" + r'''
printf '%s\n' "$@" > "$0.args"
[ ! -e "$0.fail" ] || exit 7
modules= boot=
for argument in "$@"; do
    case $argument in
        --directory=*) modules=${argument#--directory=} ;;
        --boot-directory=*) boot=${argument#--boot-directory=} ;;
    esac
done
[ -n "$modules" ] && [ -n "$boot" ]
raw=$(cat "$0.disk")
mkdir -p "$boot/grub/i386-pc"
cp "$modules/boot.img" "$boot/grub/i386-pc/boot.img"
"$grub/bin/grub-mkimage" -d "$modules" -O i386-pc -p /grub \
    -o "$boot/grub/i386-pc/core.img" biosdisk part_gpt fat normal multiboot2
printf '(hd0) %s\n' "$raw" > "$0.map"
sleep 0.1
exec "$grub/bin/grub-bios-setup" -s --root-device=hd0,gpt2 \
    -d "$boot/grub/i386-pc" -m "$0.map" "$raw"
''')
        adapter.chmod(0o755)
        helpers = {}
        for name in ("grub-bios-setup", "grub-probe", "grub-mkimage"):
            helper = work / name
            helper.write_text("#!/bin/sh\nexit 0\n")
            helper.chmod(0o755)
            helpers[name] = helper
        sources = [ROOT / "userland/runtime/src" / f"bootloader{suffix}.c"
                   for suffix in ("", "_publish", "_process")]
        subprocess.run([
            "cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
            "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror",
            f'-DBOOT_GRUB_INSTALL="{adapter}"',
            *[f'-DBOOT_{name.upper().replace("-", "_")}="{path}"' for name, path in helpers.items()],
            "-Wl,--wrap=stat,--wrap=lstat,--wrap=open,--wrap=ioctl,--wrap=fsync,--wrap=write,--wrap=setmntent",
            "-Wl,--wrap=reliefos_block_get_info,--wrap=reliefos_block_list_partitions",
            "-Wl,--wrap=reliefos_block_partition_uuid",
            "-Iuserland/runtime/include", "tests/host/test_bootloader_install.c",
            *sources, "-o", executable,
        ], cwd=ROOT, check=True)
        source = work / "source"
        for relative in ("reliefos/kernel.sys", "leonos/kernel.sys", "grub/grub.cfg",
                         "grub/fonts/leonos-unicode.pf2", "grub/theme/theme.txt", "EFI/BOOT/BOOTX64.EFI"):
            file = source / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            if relative.endswith("kernel.sys"):
                file.write_bytes(b"\x7fELF\x02\x01" + b"\0" * 58)
            elif relative.endswith(".EFI"):
                file.write_bytes(b"MZ" + b"\0" * 62)
            else:
                file.write_text("new payload\n")
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1", PATH="/nonexistent")
        for uefi in (0, 1):
            target = work / f"target-{uefi}"
            target.mkdir()
            subprocess.run([executable, source, target, "0", str(uefi), "0"], check=True, env=env)
            assert (target / "reliefos/kernel.sys").read_bytes() == (source / "reliefos/kernel.sys").read_bytes()
            assert (target / "grub/grub.cfg").read_text() == "new payload\n"
            assert (target / "EFI/BOOT/BOOTX64.EFI").exists() == bool(uefi)
            assert not (target / "grub/i386-pc").exists()
            (target / "leonos/kernel.sys").write_text("preserved rollback")
            subprocess.run([executable, source, target, "0", "0", "1"], check=True, env=env)
            assert (target / "leonos/kernel.sys").read_text() == "preserved rollback"
            assert (f"uefi={uefi}\n") in (target / "reliefos/config/bootloaders.conf").read_text()
        target = work / "unsafe"
        target.mkdir()
        outside = work / "outside"
        outside.mkdir()
        (target / "grub").symlink_to(outside, target_is_directory=True)
        result = subprocess.run([executable, source, target, "0", "1", "0"], env=env)
        assert result.returncode and list(outside.iterdir()) == []
        assert not (target / "reliefos").exists()
        (source / "EFI/BOOT/BOOTX64.EFI").unlink()
        missing_target = work / "missing"
        missing_target.mkdir()
        assert subprocess.run([executable, source, missing_target, "0", "1", "0"], env=env).returncode
        assert list(missing_target.iterdir()) == []
        assert subprocess.run([executable, source, missing_target, "1", "0", "0"], env=env).returncode
        # The metadata adapter maps Linux device nodes to owned files. Successful
        # BIOS writes are made by real locked GNU grub-bios-setup; guest tests
        # separately exercise the full upstream grub-install executable.
        shutil.copytree(grub / "lib/grub/i386-pc", source / "grub/i386-pc", copy_function=os.link)
        (source / "EFI/BOOT/BOOTX64.EFI").write_bytes(b"MZ" + b"\0" * 62)

        def disk_fixture(name):
            """Create a private standard GPT and route the real GRUB adapter to it."""
            image = work / f"{name}.raw"
            with image.open("wb") as stream:
                stream.truncate(192 * 1024**2)
            subprocess.run(["sfdisk", image], input="label: gpt\nunit: sectors\n\n"
                "start=2048,size=2048,type=21686148-6449-6E6F-744E-656564454649\n"
                "start=4096,size=262144,type=U\nstart=266240,type=L\n",
                text=True, check=True, stdout=subprocess.DEVNULL)
            Path(f"{adapter}.disk").write_text(str(image) + "\n")
            return image

        def digest_range(image, start, length):
            """Hash one independent region without loading the disk into memory."""
            digest = hashlib.sha256()
            with image.open("rb") as stream:
                stream.seek(start)
                while length:
                    chunk = stream.read(min(length, 1024**2))
                    assert chunk
                    digest.update(chunk)
                    length -= len(chunk)
            return digest.digest()

        regions = [(0, 440), (440, 1024**2 - 440), (1024**2, 1024**2),
                   (2 * 1024**2, 190 * 1024**2)]
        for bios in (0, 1):
            for uefi in (0, 1):
                name = f"matrix-{bios}-{uefi}"
                image = disk_fixture(name)
                target = work / name
                target.mkdir()
                before = [digest_range(image, *region) for region in regions]
                subprocess.run([executable, source, target, str(bios), str(uefi), "0", image, "normal"],
                               check=True, env=env)
                after = [digest_range(image, *region) for region in regions]
                assert after[1] == before[1] and after[3] == before[3]
                assert (after[0] != before[0]) == bool(bios)
                assert (after[2] != before[2]) == bool(bios)
                assert (target / "EFI/BOOT/BOOTX64.EFI").exists() == bool(uefi)
                record = (target / "reliefos/config/bootloaders.conf").read_text()
                assert record == f"version=1\nbios={bios}\nuefi={uefi}\n"
                assert (target / "reliefos/config/root.cfg").read_text() == "set reliefos_root=PARTUUID=12345678-1234-4abc-90ab-112233445566\n"
                if bios:
                    arguments = Path(f"{adapter}.args").read_text().splitlines()
                    assert "--target=i386-pc" in arguments and arguments[-1] == "/dev/sda"
                    assert "--force-file-id" in arguments
                    assert f"--boot-directory={target}" in arguments
                # Updating shared files with both modes skipped leaves both
                # loader byte ranges and the saved installed mode bits intact.
                subprocess.run([executable, source, target, "0", "0", "1", image, "normal"],
                               check=True, env=env)
                assert [digest_range(image, *region) for region in regions] == after
                assert (target / "reliefos/config/bootloaders.conf").read_text() == record
        for case in ("partition", "source-target", "wrong-disk", "no-bios-space", "bios-mounted", "bios-formatted",
                     "readonly", "copy-fail", "sync-fail", "process-fail", "missing-tool",
                     "bad-root", "missing-root", "ambiguous-root", "root-symlink", *[f"missing-{name}" for name in helpers]):
            image = disk_fixture(case)
            target = work / case
            (target / "grub").mkdir(parents=True)
            (target / "grub/grub.cfg").write_text("old menu\n")
            if case == "root-symlink":
                (target / "reliefos/config").mkdir(parents=True)
                (target / "reliefos/config/root.cfg").symlink_to(outside / "root.cfg")
            before = [digest_range(image, *region) for region in regions]
            failure = Path(f"{adapter}.fail")
            if case == "process-fail":
                failure.touch()
            if case == "missing-tool":
                adapter.chmod(0o644)
            disabled = helpers.get(case.removeprefix("missing-"))
            if disabled:
                disabled.chmod(0o644)
            try:
                result = subprocess.run([executable, source, target, "1", "1", "0", image, case], env=env)
                assert result.returncode, case
                assert (target / "grub/grub.cfg").read_text() == "old menu\n", case
                assert [digest_range(image, *region) for region in regions] == before, case
                assert not list(target.rglob(".reliefos-boot-*")), case
            finally:
                failure.unlink(missing_ok=True)
                adapter.chmod(0o755)
                if disabled:
                    disabled.chmod(0o755)
        target = work / "explicit-root"
        target.mkdir()
        image = disk_fixture("explicit-root")
        subprocess.run([executable, source, target, "0", "1", "0", image, "explicit-root"],
                       check=True, env=env)
        assert (target / "reliefos/config/root.cfg").read_text() == "set reliefos_root=PARTUUID=12345678-1234-4abc-90ab-112233445566\n"
        print("PASS boot install: all four modes, real GNU BIOS embedding boundaries, loader/rollback preservation, disk identity, copy/sync/child failures and unsafe paths")


if __name__ == "__main__":
    main()
