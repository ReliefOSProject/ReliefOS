#!/usr/bin/env python3
"""Boot a private Xorg image and verify the migrated FLTK desktop apps.

The runner never edits the selected image. It creates a sparse private copy
(using a reflink where the filesystem supports one), injects current
x86_64-musl app payloads, records
hash-checked readbacks, then drives the test/test XDM session over QMP. A new
build/fltk-migration directory is created for every invocation and retained as
reviewable evidence, including screenshots and per-window chi_sim+eng OCR.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

from PIL import Image, ImageChops, ImageOps

import test_vdso_qemu as vdso
import test_xorg_vm_qemu as xorg
from run_gcc_probe_qemu import qmp_quit
from fltk_taskmgr_gui import exercise_taskmgr

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out/xorg-vmware-debug"
BASELINE_IMAGE = ROOT / "build/fltk-migration/baseline/xorg-test.raw"
DEFAULT_IMAGE = BASELINE_IMAGE if BASELINE_IMAGE.is_file() else OUT / "images/reliefos.raw"
DEFAULT_KERNEL = OUT / "generated/system/kernel.sys"
DEFAULT_SYSROOT = OUT / "sysroot/musl"
X11_ROOT = OUT / "upstream/x11-development"
MANAGED_ROOT = OUT / "rootfs/managed"
APP_NAMES = (
    "calc", "osver", "fileman", "taskmgr", "minesweeper", "leonmmcoset",
    "xiaobai", "paint", "imageview", "doomlauncher", "doom", "installer",
)
APP_DIR = "/usr/lib/reliefos/apps"
TEST_DIR = "/usr/lib/reliefos/tests"
FIXTURE_DIR = "/home/test/桌面验收目录"
LOCALE_GUEST = "/usr/share/locale/zh_CN/LC_MESSAGES/leonos.mo"
PROBE_GUEST = TEST_DIR + "/fltk-ui-probe"
APP_RUNNER_GUEST = TEST_DIR + "/fltk-app-runner"
PROBE_RUNNER_GUEST = TEST_DIR + "/fltk-probe-runner"
FONT_PROBE_GUEST = TEST_DIR + "/fltk-font-probe"
FILEMAN_VERIFY_GUEST = TEST_DIR + "/fltk-fileman-verify"
PAINT_VERIFY_GUEST = TEST_DIR + "/fltk-paint-verify"
CHINESE_EXPECTATIONS = {
    "calc": "标准",
    "osver": "关于",
    "fileman": "新建文件夹",
    "taskmgr": "刷新",
    "minesweeper": "新游戏",
    "paint": "图片已打开",
    "imageview": "图片",
    "doomlauncher": "禁用声音",
    "installer": "安装",
}
CLIENT_OCR_ANCHORS = {
    "calc": (12, 78, 140, 26),
    "osver": (20, 46, 750, 28),
    "fileman": (12, 74, 108, 28),
    "taskmgr": (20, 66, 80, 30),
    "minesweeper": (168, 12, 154, 32),
    "paint": (12, 660, 916, 28),
    "doomlauncher": (18, 164, 290, 30),
}
# Keep button anchors auditable at their full client bounds, then exclude the
# bevel for OCR when its dotted focus border confuses line segmentation.
CLIENT_OCR_TEXT_INSETS = {
    "minesweeper": (42, 4, 70, 24),
}
READY_MARKERS = {
    "calc": "[calc.elf] FLTK calculator ready",
    "osver": "[osver.elf] FLTK system information ready",
    "fileman": "[fileman.elf] FLTK file manager ready",
    "taskmgr": "[taskmgr.elf] FLTK task manager ready",
    "minesweeper": "[minesweeper.elf] FLTK minesweeper ready",
    "leonmmcoset": "[leonmmcoset.elf] FLTK image ready",
    "xiaobai": "[xiaobai.elf] FLTK image ready",
    "paint": "[paint.elf] FLTK paint ready",
    "imageview": "[imageview.elf] FLTK image viewer ready",
    "doomlauncher": "[doomlauncher.elf] FLTK DOOM launcher ready",
    "doom": "[doom.elf] FLTK game frame ready",
    "installer": "[installer.elf] FLTK installer ready",
}
DOOM_SECONDS = 60
PNG_CONTENT_MATCH_MIN_FRACTION = 0.995
PNG_PIXEL_CHANNEL_TOLERANCE = 4


def run(command: list[object], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(item) for item in command], cwd=ROOT, check=True,
                          env={**os.environ, "TMPDIR": str(ROOT / "out/tmp")}, **kwargs)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_target_elf(elf: Path, *, sysroot: Path,
                        required_needed: tuple[str, ...] = (),
                        require_interpreter: bool = True) -> set[str]:
    """Reject host-ABI artifacts before they reach the private guest disk."""
    headers = subprocess.check_output(["readelf", "-l", elf], cwd=ROOT, text=True)
    dynamic = subprocess.check_output(["readelf", "-d", elf], cwd=ROOT, text=True)
    versions = subprocess.check_output(["readelf", "-V", elf], cwd=ROOT, text=True)
    interpreter = "/lib/ld-musl-x86_64.so.1"
    if require_interpreter and interpreter not in headers:
        raise RuntimeError(f"{elf} does not use the pinned musl interpreter")
    if "ld-linux" in headers or "libc.so.6" in dynamic or re.search(r"GLIBC_[0-9]", versions):
        raise RuntimeError(f"{elf} contains a host glibc dependency")
    if str(sysroot.resolve()) in dynamic:
        raise RuntimeError(f"{elf} leaks a build sysroot path into its dynamic section")
    needed = set(re.findall(r"Shared library: \[(.*?)\]", dynamic))
    missing = set(required_needed) - needed
    if missing:
        raise RuntimeError(f"{elf} is missing required DT_NEEDED entries: {sorted(missing)}")
    return needed


def target_compile_probe(output: Path, sysroot: Path, cxx: str, ld: str,
                         *, build_dir: Path | None = None) -> Path:
    """Build only the smoke probe, with the same musl/FLTK boundary as apps."""
    probe_build = OUT if build_dir is None else build_dir
    x11_root = X11_ROOT if build_dir is None else build_dir / "upstream/x11-development"
    managed_root = MANAGED_ROOT if build_dir is None else build_dir / "rootfs/managed"
    resource_dir = Path(subprocess.check_output(
        [cxx, "-print-resource-dir"], cwd=ROOT, text=True).strip())
    source = ROOT / "tools/tests/fltk_ui_probe.cpp"
    obj = output / "fltk_ui_probe.o"
    elf = output / "fltk_ui_probe.elf"
    build_log = output / "probe-build.log"
    command_log = output / "probe-build.command.json"
    compile_command = [
        cxx, "--target=x86_64-linux-musl", "-O0", "-g", "-fPIE",
        "-nostdinc", "-nostdinc++", "-isystem", sysroot / "include",
        "-isystem", resource_dir / "include", "-isystem", x11_root / "usr/include",
        "-I", ROOT / "userland/apps", "-I", ROOT / "include",
        "-I", probe_build / "include", "-I", probe_build / "sdk/reliefos-musl-sdk/include",
        "-D_POSIX_C_SOURCE=200809L", "-D_GNU_SOURCE", "-std=c++17",
        "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
        "-include", probe_build / "include/generated/autoconf.h", "-c", source,
        "-o", obj,
    ]
    link_command = [
        ld, "--gc-sections", "-z", "max-page-size=0x1000", "-pie",
        "--hash-style=both", "--dynamic-linker", "/lib/ld-musl-x86_64.so.1",
        "-rpath", "/usr/lib/reliefos:/lib:/usr/lib", "--build-id=none",
        "-o", elf, sysroot / "lib/Scrt1.o", sysroot / "lib/crti.o", obj,
        "-L", sysroot / "lib", "-l:libmimalloc.so.3", "--start-group",
        x11_root / "usr/lib/libfltk.so", x11_root / "usr/lib/libstdc++.so.6",
        x11_root / "usr/lib/libgcc_s.so.1",
        managed_root / "usr/lib/libX11.so.6", "-lc", "--end-group",
        sysroot / "lib/crtn.o",
    ]
    command_log.write_text(json.dumps({"compile": [str(x) for x in compile_command],
                                       "link": [str(x) for x in link_command]}, indent=2) + "\n",
                           encoding="utf-8")
    with build_log.open("w", encoding="utf-8") as log:
        run(compile_command, stdout=log, stderr=subprocess.STDOUT)
        run(link_command, stdout=log, stderr=subprocess.STDOUT)

    validate_target_elf(elf, sysroot=sysroot,
                        required_needed=("libfltk.so.1.3", "libstdc++.so.6"))
    return elf


def debugfs_quote(value: str | Path) -> str:
    text = str(value)
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generated_guest_helpers(output: Path) -> dict[str, Path]:
    helpers = output / "payload"
    helpers.mkdir()
    app_runner = helpers / "fltk-app-runner"
    app_runner.write_text(
        "#!/bin/sh\n"
        "export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8\n"
        "app=$1\n"
        "shift\n"
        "case \"$app\" in calc|osver|fileman|taskmgr|minesweeper|leonmmcoset|xiaobai|paint|imageview|doomlauncher|doom|installer) ;; *) echo '[fltk-runner] FAIL unknown app' > /dev/ttyS0; exit 64;; esac\n"
        f"exe={APP_DIR}/$app/$app.elf\n"
        "case \"$app\" in\n"
        f"  fileman) set -- '{FIXTURE_DIR}' ;;\n"
        f"  imageview) set -- '{FIXTURE_DIR}/图片标志.png' ;;\n"
        f"  paint) set -- '{FIXTURE_DIR}/扫雷矿石.bmp' ;;\n"
        "esac\n"
        "if [ \"$app\" = installer ]; then set -- --graphical; fi\n"
        "if [ \"$app\" = taskmgr ]; then\n"
        "  (\n"
        "    /bin/sleep 600 &\n"
        "    worker=$!\n"
        "    echo \"FLTK_TASKMGR_WORKER pid=$worker\" > /dev/ttyS0\n"
        "    wait \"$worker\"\n"
        "    worker_status=$?\n"
        "    echo \"FLTK_TASKMGR_WORKER_EXIT pid=$worker status=$worker_status\" > /dev/ttyS0\n"
        "  ) &\n"
        "fi\n"
        f"if [ \"$app\" = doom ]; then\n"
        f"  /bin/sh -c 'app=$1; shift; echo \"[fltk-runner] START app=$app pid=$$\" > /dev/ttyS0; exec timeout {DOOM_SECONDS} \"$@\"' fltk-child \"$app\" \"$exe\" \"$@\" >/dev/ttyS0 2>&1 &\n"
        "else\n"
        "  /bin/sh -c 'app=$1; shift; echo \"[fltk-runner] START app=$app pid=$$\" > /dev/ttyS0; exec \"$@\"' fltk-child \"$app\" \"$exe\" \"$@\" >/dev/ttyS0 2>&1 &\n"
        "fi\n"
        "pid=$!\n"
        'case "$app" in\n'
        f'  fileman|taskmgr) {PROBE_GUEST} --watch-window "$pid" >/dev/ttyS0 2>&1 & ;;\n'
        f'  doom) ;; *) {PROBE_GUEST} --window-info "$pid" >/dev/ttyS0 2>&1 ;;\n'
        'esac\n'
        "wait \"$pid\"\n"
        "status=$?\n"
        "echo \"[fltk-runner] EXIT app=$app pid=$pid status=$status\" > /dev/ttyS0\n"
        "exit 0\n",
        encoding="utf-8",
    )
    probe_runner = helpers / "fltk-probe-runner"
    probe_runner.write_text(
        "#!/bin/sh\nexport LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8\n"
        f"/bin/sh -c 'echo \"[fltk-runner] START app=fltk-ui-probe pid=$$\" > /dev/ttyS0; exec \"$@\"' fltk-probe-child {PROBE_GUEST} >/dev/ttyS0 2>&1 &\n"
        "pid=$!\n"
        "wait \"$pid\"\n"
        "status=$?\n"
        "echo \"[fltk-runner] EXIT app=fltk-ui-probe pid=$pid status=$status\" > /dev/ttyS0\n"
        "exit 0\n", encoding="ascii")
    font_probe = helpers / "fltk-font-probe"
    font_probe.write_text(
        "#!/bin/sh\n"
        "export LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8\n"
        "echo FLTK_FONT_BEGIN > /dev/ttyS0\n"
        "if command -v fc-match >/dev/null 2>&1; then\n"
        "  fc-match -f 'FLTK_FONT family=%{family};file=%{file};charset=%{charset}\\n' SimSun > /dev/ttyS0 2>&1\n"
        "  result=$?\n"
        "else\n"
        "  echo 'FLTK_FONT_ERROR missing fc-match' > /dev/ttyS0\n"
        "  result=127\n"
        "fi\n"
        "echo FLTK_FONT_END result=$result > /dev/ttyS0\n"
        "exit 0\n", encoding="ascii")
    fileman_verify = helpers / "fltk-fileman-verify"
    fileman_verify.write_text(
        "#!/bin/sh\n"
        f"base='{FIXTURE_DIR}'\n"
        'source="$base/中文样本_renamed.txt"\n'
        'copy="$base/roundtrip/中文样本_renamed.txt"\n'
        'if [ -d "$base/roundtrip" ] && [ -f "$source" ] && [ -f "$copy" ] && '
        '[ ! -e "$base/中文样本.txt" ] && cmp -s "$source" "$copy"; then\n'
        "  echo FLTK_FILEMAN_VERIFY PASS > /dev/ttyS0\n"
        "else\n"
        "  echo FLTK_FILEMAN_VERIFY FAIL > /dev/ttyS0\n"
        "fi\n", encoding="utf-8")
    paint_verify = helpers / "fltk-paint-verify"
    paint_verify.write_text(
        "#!/bin/sh\n"
        f"current='{FIXTURE_DIR}/扫雷矿石.bmp'\n"
        "original='/usr/share/reliefos/resources/minesweeper-mine.bmp'\n"
        'if [ -s "$current" ] && [ -s "$original" ] && ! cmp -s "$current" "$original"; then\n'
        "  echo FLTK_PAINT_VERIFY PASS > /dev/ttyS0\n"
        "else\n"
        "  echo FLTK_PAINT_VERIFY FAIL > /dev/ttyS0\n"
        "fi\n", encoding="utf-8")
    return {"app_runner": app_runner, "probe_runner": probe_runner,
            "font_probe": font_probe, "fileman_verify": fileman_verify,
            "paint_verify": paint_verify}


def materialize_fixtures(output: Path) -> list[tuple[Path, str, int]]:
    fixture_dir = output / "fixtures"
    fixture_dir.mkdir()
    text_file = fixture_dir / "中文样本.txt"
    text_file.write_text("桌面中文目录中的 UTF-8 验收文件。\n第二行：中文显示与路径读取正常。\n",
                         encoding="utf-8")
    png_file = fixture_dir / "图片标志.png"
    shutil.copyfile(ROOT / "logo.png", png_file)
    mine_source = ROOT / "resources/build-art/minesweeper/minesweeper-mine.bmp"
    mine_file = fixture_dir / "扫雷矿石.bmp"
    shutil.copyfile(mine_source, mine_file)
    flag_source = ROOT / "resources/build-art/minesweeper/minesweeper-flag.bmp"
    flag_file = fixture_dir / "扫雷旗帜.bmp"
    shutil.copyfile(flag_source, flag_file)
    search_dir = fixture_dir / ".search-fixture" / "子目录"
    search_dir.mkdir(parents=True)
    # Keep the locate target inside the ordinary browser's legacy 64-row
    # window while the independent BulkNeedle search still exercises 80 hits.
    search_target = search_dir / "000-深层DeepSearchNeedle.txt"
    search_target.write_text("用于递归搜索与中文路径验收。\n", encoding="utf-8")
    search_files = [
        (search_target,
         FIXTURE_DIR + "/.search-fixture/子目录/000-深层DeepSearchNeedle.txt",
         0o100664),
    ]
    for index in range(80):
        bulk_file = search_dir / f"BulkNeedle{index:03d}.txt"
        bulk_file.write_text("用于超过 64 条结果的搜索验收。\n", encoding="utf-8")
        search_files.append((
            bulk_file,
            FIXTURE_DIR + f"/.search-fixture/子目录/BulkNeedle{index:03d}.txt",
            0o100664,
        ))
    return [
        (text_file, FIXTURE_DIR + "/中文样本.txt", 0o100664),
        (png_file, FIXTURE_DIR + "/图片标志.png", 0o100664),
        (mine_file, FIXTURE_DIR + "/扫雷矿石.bmp", 0o100664),
        (flag_file, FIXTURE_DIR + "/扫雷旗帜.bmp", 0o100664),
    ] + search_files


def payload_files(output: Path, probe: Path) -> list[tuple[Path, str, int]]:
    items: list[tuple[Path, str, int]] = []
    for app in APP_NAMES:
        elf = OUT / "userland" / f"{app}.elf"
        if not elf.is_file():
            raise FileNotFoundError(f"current app ELF is missing: {elf}")
        validate_target_elf(
            elf, sysroot=OUT / "sysroot/musl",
            required_needed=("libfltk.so.1.3", "libstdc++.so.6", "libgcc_s.so.1"))
        items.append((elf, f"{APP_DIR}/{app}/{app}.elf", 0o100755))

        icon = ROOT / "resources/build-art/app-icons" / f"{app}.bmp"
        if icon.is_file():
            items.append((icon, f"{APP_DIR}/{app}/{app}.bmp", 0o100644))

        image = ROOT / "userland/apps" / app / f"{app}.png"
        if image.is_file():
            items.append((image, f"{APP_DIR}/{app}/{app}.png", 0o100644))

    wad = MANAGED_ROOT / "usr/lib/reliefos/apps/doom/freedoom1.wad"
    if not wad.is_file():
        raise FileNotFoundError(f"DOOM IWAD fixture is missing: {wad}")
    items.append((wad, f"{APP_DIR}/doom/freedoom1.wad", 0o100644))
    opl3 = OUT / "upstream/nuked-opl3/root/usr/lib/libopl3.so.1"
    if not opl3.is_file():
        raise FileNotFoundError(f"current DOOM audio runtime is missing: {opl3}")
    sysroot = OUT / "sysroot/musl"
    validate_target_elf(opl3, sysroot=sysroot, require_interpreter=False)
    items.append((opl3, "/usr/lib/libopl3.so.1", 0o100755))
    current_runtime = OUT / "system/lib/libreliefos.so.2"
    current_compat = OUT / "system/lib/libleonos.so.2"
    if not current_runtime.is_file() or not current_compat.is_file():
        raise FileNotFoundError("current ReliefOS runtime/compatibility libraries are missing")
    validate_target_elf(current_runtime, sysroot=sysroot, require_interpreter=False)
    validate_target_elf(current_compat, sysroot=sysroot, require_interpreter=False)
    items.append((current_runtime, "/usr/lib/reliefos/libreliefos.so.2", 0o100755))
    items.append((current_compat, "/usr/lib/leonos/libleonos.so.2", 0o100755))
    items.extend([
        (sysroot / "lib/ld-musl-x86_64.so.1", "/lib/ld-musl-x86_64.so.1", 0o100755),
        (sysroot / "lib/libc.so", "/lib/libc.so", 0o100755),
        (sysroot / "lib/libmimalloc.so.3", "/lib/libmimalloc.so.3", 0o100755),
    ])
    for source, guest in (
        # Place the current APK bytes under the SONAMEs embedded in the app
        # ELFs. The package filenames are versioned and lack their symlinks.
        (X11_ROOT / "usr/lib/libfltk.so.1.3.11", "/usr/lib/libfltk.so.1.3"),
        (X11_ROOT / "usr/lib/libstdc++.so.6.0.34", "/usr/lib/libstdc++.so.6"),
        (X11_ROOT / "usr/lib/libgcc_s.so.1", "/usr/lib/libgcc_s.so.1"),
    ):
        if not source.is_file():
            raise FileNotFoundError(f"current APK runtime library is missing: {source}")
        validate_target_elf(source, sysroot=sysroot, require_interpreter=False)
        items.append((source, guest, 0o100755))
    for library in (sysroot / "lib/libc.so", sysroot / "lib/libmimalloc.so.3"):
        validate_target_elf(library, sysroot=sysroot, require_interpreter=False)
    items.append((ROOT / "logo.png", "/usr/share/reliefos/resources/logo.png", 0o100644))
    for name in ("minesweeper-mine.bmp", "minesweeper-flag.bmp"):
        source = ROOT / "resources/build-art/minesweeper" / name
        items.append((source, f"/usr/share/reliefos/resources/{name}", 0o100644))

    catalog = OUT / "generated/nls/zh_CN/LC_MESSAGES/leonos.mo"
    if not catalog.is_file():
        raise FileNotFoundError(f"current zh_CN catalog is missing: {catalog}")
    items.append((catalog, LOCALE_GUEST, 0o100644))
    items.append((probe, PROBE_GUEST, 0o100755))
    helpers = generated_guest_helpers(output)
    items.extend([
        (helpers["app_runner"], APP_RUNNER_GUEST, 0o100755),
        (helpers["probe_runner"], PROBE_RUNNER_GUEST, 0o100755),
        (helpers["font_probe"], FONT_PROBE_GUEST, 0o100755),
        (helpers["fileman_verify"], FILEMAN_VERIFY_GUEST, 0o100755),
        (helpers["paint_verify"], PAINT_VERIFY_GUEST, 0o100755),
    ])
    items.extend(materialize_fixtures(output))
    return items


def inject_payload(root_image: Path, output: Path,
                   files: list[tuple[Path, str, int]]) -> list[dict[str, object]]:
    commands: list[str] = []
    for _, guest, _ in files:
        parent = str(PurePosixPath(guest).parent)
        vdso.add_missing_root_directory(root_image, parent, commands)
    for _, guest, _ in files:
        if vdso.debugfs_exists(root_image, guest):
            commands.append(f"rm {guest}")
    for source, guest, mode in files:
        commands.extend([
            f"write {debugfs_quote(source.resolve())} {debugfs_quote(guest)}",
            f"set_inode_field {debugfs_quote(guest)} mode {mode:07o}",
        ])
        if PurePosixPath(guest).parent == PurePosixPath(FIXTURE_DIR):
            commands.extend([
                f"set_inode_field {debugfs_quote(guest)} uid 1000",
                f"set_inode_field {debugfs_quote(guest)} gid 1000",
            ])
    commands.extend([
        f"set_inode_field {debugfs_quote(FIXTURE_DIR)} mode 040775",
        f"set_inode_field {debugfs_quote(FIXTURE_DIR)} uid 1000",
        f"set_inode_field {debugfs_quote(FIXTURE_DIR)} gid 1000",
    ])
    command_path = output / "debugfs.commands"
    command_path.write_text("\n".join(commands) + "\n", encoding="utf-8")
    with (output / "debugfs-inject.log").open("w", encoding="utf-8") as log:
        run(["debugfs", "-w", "-f", command_path, root_image],
            stdout=log, stderr=subprocess.STDOUT)

    readbacks = output / "readbacks"
    readbacks.mkdir()
    manifest: list[dict[str, object]] = []
    for index, (source, guest, mode) in enumerate(files):
        readback = readbacks / f"{index:03d}-{source.name.encode('utf-8').hex()[:24]}"
        run(["debugfs", "-R", f"dump {debugfs_quote(guest)} {debugfs_quote(readback)}",
             root_image], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        source_hash = sha256(source)
        readback_hash = sha256(readback)
        if source_hash != readback_hash:
            raise RuntimeError(f"debugfs readback hash mismatch: {guest}")
        manifest.append({
            "source": str(source.resolve()), "guest_path": guest,
            "mode_octal": f"{mode:07o}", "size": source.stat().st_size,
            "sha256": source_hash, "readback_sha256": readback_hash,
        })
    return manifest


def clone_private_raw(image: Path, output: Path) -> Path:
    raw = output / "guest-private.raw"
    run(["cp", "--reflink=auto", "--sparse=always", "--preserve=mode,timestamps",
         image.resolve(), raw])
    if sha256(image) != sha256(raw):
        raise RuntimeError("private disk differs from the source image")
    return raw


def prepare_disk(image: Path, kernel: Path, output: Path,
                 payload: list[tuple[Path, str, int]]) -> dict[str, object]:
    image_hash_before = sha256(image)
    raw = clone_private_raw(image, output)
    partitions = vdso.gpt_partitions(raw)
    esp = next((entry for entry in partitions if entry["name"] == "RELIEFOS_ESP"), None)
    root = next((entry for entry in partitions if entry["name"] == "RELIEFOS_ROOT"), None)
    if esp is None or root is None:
        raise ValueError("raw image needs RELIEFOS_ESP and RELIEFOS_ROOT GPT partitions")
    layout: dict[str, int | str] = {
        "esp_first": int(esp["first"]), "root_first": int(root["first"]),
        "root_last": int(root["last"]),
    }
    root_image = vdso.extract_root(raw, layout, output)
    injected = inject_payload(root_image, output, payload)
    vdso.put_root_back(raw, root_image, layout)
    kernel_hashes = vdso.install_private_kernel(raw, int(layout["esp_first"]), kernel, output)
    image_hash_after = sha256(image)
    if image_hash_before != image_hash_after:
        raise RuntimeError("source image changed while preparing private disk")
    return {
        "source_image": str(image), "source_image_sha256_before": image_hash_before,
        "source_image_sha256_after": image_hash_after,
        "source_image_unchanged": True, "private_image": str(raw),
        "private_image_sha256": sha256(raw), "root_image_snapshot": str(root_image),
        "gpt_partitions": partitions, "kernel": str(kernel),
        "kernel_hashes": kernel_hashes, "injected_files": injected,
    }


def tesseract_ocr(frame, output: Path, tag: str, psm: int = 11,
                  language: str = "chi_sim+eng", scale: int = 2) -> str:
    scratch = ROOT / "out/tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=f"fltk-{tag}-", suffix=".png",
                                     dir=scratch) as temporary:
        frame.resize((frame.width * scale, frame.height * scale)).save(temporary.name)
        text = subprocess.check_output(
            ["tesseract", temporary.name, "stdout", "-l", language,
             "--psm", str(psm)], cwd=ROOT, text=True, stderr=subprocess.DEVNULL)
    (output / f"{tag}.ocr.txt").write_text(text, encoding="utf-8")
    return text


def cjk_count(text: str) -> int:
    return len(re.findall(r"[\u3400-\u4dbf\u4e00-\u9fff]", text))


def client_crop(frame, x: int, y: int, width: int, height: int):
    left, top = max(0, x), max(0, y)
    right, bottom = min(frame.width, x + width), min(frame.height, y + height)
    if right <= left or bottom <= top:
        raise AssertionError(
            f"client geometry is outside screenshot: {(x, y, width, height)} "
            f"within {(frame.width, frame.height)}")
    return frame.crop((left, top, right, bottom))


def frame_distance(left, right, size: tuple[int, int] = (160, 90)) -> float:
    before = left.resize(size).convert("L")
    after = right.resize(size).convert("L")
    pixels = size[0] * size[1]
    return sum(abs(before.getpixel((x, y)) - after.getpixel((x, y)))
               for y in range(size[1]) for x in range(size[0])) / (pixels * 255)


def expected_png_client(app: str, canvas_width: int, canvas_height: int):
    """Render the PNG canvas using the app's fit_rect and FLTK pixel callback."""
    source_path = ROOT / "userland/apps" / app / f"{app}.png"
    source = Image.open(source_path).convert("RGBA")
    source_width, source_height = source.size
    margin = 8
    available_width = canvas_width - margin * 2
    available_height = canvas_height - margin * 2
    if min(source_width, source_height, available_width, available_height) <= 0:
        raise AssertionError(f"{app}: invalid source/canvas dimensions for PNG verification")

    draw_width = available_width
    draw_height = draw_width * source_height // source_width
    if draw_height > available_height:
        draw_height = available_height
        draw_width = draw_height * source_width // source_height
    if min(draw_width, draw_height) <= 0:
        raise AssertionError(f"{app}: fit_rect produced an empty PNG destination")
    draw_x = (canvas_width - draw_width) // 2
    draw_y = (canvas_height - draw_height) // 2

    # reliefos_png_decode_file converts to RGB and composites any alpha over
    # white. fltk_pixels.h then samples floor(dst * source / draw_size).
    source_bytes = source.tobytes()
    client_bytes = bytearray(canvas_width * canvas_height * 3)
    for y in range(draw_height):
        source_y = y * source_height // draw_height
        for x in range(draw_width):
            source_x = x * source_width // draw_width
            source_offset = (source_y * source_width + source_x) * 4
            red, green, blue, alpha = source_bytes[source_offset:source_offset + 4]
            inverse_alpha = 255 - alpha
            destination_offset = ((draw_y + y) * canvas_width + draw_x + x) * 3
            client_bytes[destination_offset:destination_offset + 3] = bytes((
                (red * alpha + 255 * inverse_alpha + 127) // 255,
                (green * alpha + 255 * inverse_alpha + 127) // 255,
                (blue * alpha + 255 * inverse_alpha + 127) // 255,
            ))
    expected = Image.frombytes("RGB", (canvas_width, canvas_height), bytes(client_bytes))
    return source_path, expected, (draw_x, draw_y, draw_width, draw_height)


def png_content_match(actual, expected, image_rect: tuple[int, int, int, int],
                      tolerance: int = 4) -> tuple[float, float]:
    """Return fraction of source-image pixels within tolerance and mean max error."""
    x, y, width, height = image_rect
    actual_image = actual.crop((x, y, x + width, y + height)).convert("RGB")
    expected_image = expected.crop((x, y, x + width, y + height)).convert("RGB")
    difference = ImageChops.difference(actual_image, expected_image)
    maximum_channel_difference = ImageChops.lighter(
        ImageChops.lighter(difference.getchannel("R"), difference.getchannel("G")),
        difference.getchannel("B"))
    histogram = maximum_channel_difference.histogram()
    pixels = width * height
    matching = sum(histogram[:tolerance + 1]) / pixels
    mean_error = sum(value * count for value, count in enumerate(histogram)) / pixels
    return matching, mean_error


def require_chinese_probe_ocr(text: str) -> None:
    anchors = ("剪贴板验收", "密码输入框")
    missing = [anchor for anchor in anchors if not normalized_contains(text, anchor)]
    if cjk_count(text) < 5 or missing:
        raise AssertionError(
            f"Tesseract did not recognize independent probe labels {missing!r}: {text!r}")


def normalized_contains(text: str, expected: str) -> bool:
    compact = re.sub(r"[\s\W_]+", "", text, flags=re.UNICODE).lower()
    return re.sub(r"[\s\W_]+", "", expected, flags=re.UNICODE).lower() in compact


def font_match_record(serial_text: str) -> dict[str, str]:
    matches = re.findall(
        r"FLTK_FONT family=(.*?);file=(.*?);charset=(.*)", serial_text)
    if not matches:
        raise AssertionError("fc-match did not report family, file, and charset")
    family, path, charset = matches[-1]
    codepoint = 0x4E2D
    covers_chinese = any(
        int(start, 16) <= codepoint <= int(end or start, 16)
        for start, end in re.findall(
            r"(?<![0-9a-f])([0-9a-f]{2,6})(?:-([0-9a-f]{2,6}))?",
            charset.lower())
    )
    result = {"family": family, "file": path, "charset": charset,
              "covers_u4e2d": str(covers_chinese).lower()}
    if "simsun" not in family.lower() or "simsun.ttc" not in path.lower() or not covers_chinese:
        raise AssertionError(f"SimSun match lacks expected family/file/charset: {result}")
    return result


def wait_marker(serial: Path, marker: str, process, offset: int, timeout: float) -> str:
    xorg.wait_serial_after(serial, marker, process, offset, timeout=timeout)
    return serial.read_text(errors="replace")


def focus_center(probe) -> None:
    probe.click(probe.width // 2, probe.height // 2)


def probe_chinese_input(probe, serial: Path, process, output: Path,
                        failures: list[str]) -> dict[str, object]:
    start = len(serial.read_text(errors="replace")) if serial.exists() else 0
    xorg.st_marker(probe, serial, process, "FLTK_PROBE_READY",
                   f"{PROBE_RUNNER_GUEST}")
    text = wait_marker(serial, "FLTK_PROBE_READY", process, start, 20)
    recent = text[start:]
    start_line = next((line for line in recent.splitlines()
                       if "[fltk-runner] START app=fltk-ui-probe " in line), "")
    pid_match = re.search(r"\bpid=(\d+)", start_line)
    if not pid_match:
        raise AssertionError(f"probe runner start marker omitted PID: {start_line!r}")
    pid = int(pid_match.group(1))
    geometry = re.search(r"FLTK_PROBE_GEOMETRY x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", recent)
    if not geometry:
        raise AssertionError("probe omitted mapped client geometry")
    window_x, window_y, window_w, window_h = map(int, geometry.groups())
    time.sleep(0.5)
    initial = probe.frame("fltk-probe-ready")
    initial_client = client_crop(initial, window_x, window_y, window_w, window_h)
    initial_client.save(output / "fltk-probe-ready.client.png")
    ocr_text = tesseract_ocr(initial_client, output, "fltk-probe-ready")
    heading_crop = client_crop(initial, window_x + 24, window_y + 16, 802, 38)
    secret_crop = client_crop(initial, window_x + 28, window_y + 132, 214, 34)
    heading_text = tesseract_ocr(heading_crop, output, "fltk-probe-heading", psm=7)
    secret_text = tesseract_ocr(secret_crop, output, "fltk-probe-password-label", psm=7)
    try:
        require_chinese_probe_ocr(heading_text + "\n" + secret_text)
    except AssertionError as error:
        failures.append(str(error))

    input_point = (window_x + 535, window_y + 99)
    secret_point = (window_x + 535, window_y + 149)
    browser_point = (window_x + 150, window_y + 240)

    offset = len(serial.read_text(errors="replace"))
    probe.click(*input_point)
    probe.key("ctrl-v")
    wait_marker(serial, "FLTK_PROBE_PASS input_utf8_paste", process, offset, 10)

    offset = len(serial.read_text(errors="replace"))
    probe.click(*secret_point)
    probe.key("ctrl-v")
    wait_marker(serial, "FLTK_PROBE_PASS secret_utf8_paste", process, offset, 10)

    offset = len(serial.read_text(errors="replace"))
    probe.click(*browser_point)
    wait_marker(serial, "FLTK_PROBE_PASS browser_utf8_selection", process, offset, 10)
    screenshot = probe.frame("fltk-probe-utf8-paste-and-browser")
    markers = serial.read_text(errors="replace")
    copy_accept = "FLTK_PROBE_PASS copy_input_accept" in markers
    copy_reject = "FLTK_PROBE_PASS copy_input_reject_unchanged" in markers
    if not copy_accept or not copy_reject:
        raise AssertionError("probe did not validate both copy_input capacity cases")
    return {"screenshot": str(output / "fltk-probe-utf8-paste-and-browser.png"),
            "client_screenshot": str(output / "fltk-probe-ready.client.png"),
            "ocr_file": str(output / "fltk-probe-ready.ocr.txt"),
            "anchor_ocr_files": [
                str(output / "fltk-probe-heading.ocr.txt"),
                str(output / "fltk-probe-password-label.ocr.txt"),
            ],
            "pid": pid, "copy_input_capacity_accept": copy_accept,
            "copy_input_capacity_reject_unchanged": copy_reject, "input_paste": True,
            "secret_paste": True, "browser_selection": True,
            "screen_size": [screenshot.width, screenshot.height]}


def run_font_probe(probe, serial: Path, process) -> dict[str, str]:
    offset = len(serial.read_text(errors="replace"))
    xorg.st_marker(probe, serial, process, "FLTK_FONT_END",
                   FONT_PROBE_GUEST)
    text = wait_marker(serial, "FLTK_FONT_END", process, offset, 15)
    return font_match_record(text)


def click_client(probe, geometry: tuple[int, int, int, int], x: int, y: int) -> None:
    probe.click(geometry[0] + x, geometry[1] + y)


def fileman_address_crop(frame, geometry: tuple[int, int, int, int]):
    window_x, window_y, window_width, _ = geometry
    return client_crop(frame, window_x + 204, window_y + 36,
                       window_width - 416, 30)


def wait_client_geometry(serial: Path, process, pid: int, offset: int,
                         width: int | None = None, height: int | None = None):
    pattern = re.compile(r"FLTK_APP_GEOMETRY pid=" + str(pid) +
                         r" x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        matches = list(pattern.finditer(serial.read_text(errors="replace")[offset:]))
        for match in reversed(matches):
            geometry = tuple(map(int, match.groups()))
            # Absolute USB tablet coordinates are quantized to screen
            # pixels, so native border drags can differ by a pixel or two.
            if ((width is None or abs(geometry[2] - width) <= 2) and
                    (height is None or abs(geometry[3] - height) <= 2)):
                return geometry
        if process.poll() is not None:
            raise RuntimeError("QEMU exited during window resize")
        time.sleep(0.1)
    raise AssertionError(f"window {pid} did not report client geometry {width}x{height}")


def double_click(probe, x: int, y: int) -> None:
    probe.move(x, y)
    for _ in range(2):
        probe.button(True)
        time.sleep(0.06)
        probe.button(False)
        time.sleep(0.08)
    time.sleep(0.35)


def exercise_window_resizing(app: str, probe, geometry, serial: Path,
                             process, output: Path, pid: int):
    original = geometry
    default = (960, 600) if app == "fileman" else (1000, 660)
    minimum = (800, 480) if app == "fileman" else (800, 520)
    if (abs(original[2] - default[0]) > 2 or
            abs(original[3] - default[1]) > 2):
        raise AssertionError(
            f"{app}: unexpected default client geometry {original[2:]} (expected {default})")
    evidence = {"default": list(default), "minimum": list(minimum), "captures": []}

    def capture(tag, actual):
        frame = probe.frame(tag)
        client = client_crop(frame, *actual)
        path = output / f"{tag}.client.png"
        client.save(path)
        if client.size != actual[2:]:
            raise AssertionError(f"{app}: resized window is clipped by the display")
        if app == "fileman":
            label = client_crop(client, 12, 74, 108, 28)
            search = client_crop(client, actual[2] - 204, 36, 92, 30)
            text = tesseract_ocr(label, output, tag + "-new-folder", psm=7)
            search_text = tesseract_ocr(search, output, tag + "-search", psm=7)
            if (not normalized_contains(text, "新建文件夹") or
                    not normalized_contains(search_text, "搜索")):
                raise AssertionError(f"{app}: controls were lost after resize: {text!r}, {search_text!r}")
        else:
            headers = client_crop(client, 20, 108, actual[2] - 40, 24)
            text = tesseract_ocr(headers, output, tag + "-columns", psm=7)
            if not normalized_contains(text, "PID") or cjk_count(text) < 4:
                raise AssertionError(f"{app}: process columns were lost after resize: {text!r}")
        evidence["captures"].append({"geometry": list(actual), "screenshot": str(path)})

    def drag_resize(current, width, height):
        offset = len(serial.read_text(errors="replace"))
        probe.move(current[0] + current[2] + 2, current[1] + current[3] + 2)
        probe.button(True)
        time.sleep(0.15)
        probe.move(current[0] + width + 2, current[1] + height + 2)
        time.sleep(0.25)
        probe.button(False)
        actual = wait_client_geometry(serial, process, pid, offset, width, height)
        time.sleep(0.3)
        return actual

    geometry = drag_resize(geometry, *minimum)
    if geometry[2] < minimum[0] or geometry[3] < minimum[1]:
        raise AssertionError(f"{app}: window resized below its declared minimum: {geometry}")
    capture(f"app-{app}-minimum", geometry)
    geometry = drag_resize(geometry, original[2], original[3])
    capture(f"app-{app}-restored", geometry)
    offset = len(serial.read_text(errors="replace"))
    double_click(probe, geometry[0] + 200, geometry[1] - 12)
    maximized = wait_client_geometry(serial, process, pid, offset)
    if maximized[2] <= original[2] or maximized[3] < original[3]:
        raise AssertionError(f"{app}: title-bar double-click did not maximize: {maximized}")
    capture(f"app-{app}-maximized", maximized)
    offset = len(serial.read_text(errors="replace"))
    double_click(probe, maximized[0] + 200, maximized[1] - 12)
    geometry = wait_client_geometry(serial, process, pid, offset, original[2], original[3])
    time.sleep(0.25)
    evidence["restored_geometry"] = list(geometry)
    evidence["passed"] = True
    return geometry, evidence


def fileman_search_phase(text: str) -> str | None:
    compact = re.sub(r"[\s\W_]+", "", text, flags=re.UNICODE).lower()
    if "limit" in compact or "上限" in compact:
        return "limited"
    if "searching" in compact or "正在搜索" in compact or "搜索中" in compact:
        return "searching"
    if "searchstopped" in compact or "停止" in compact:
        return "stopped"
    if "partial" in compact or "部分" in compact:
        return "partial"
    if "searchcomplete" in compact or "搜索完成" in compact:
        return "complete"
    if "nomatchesfound" in compact or "没有匹配" in compact or "未找到" in compact:
        return "empty"
    return None


def fileman_search_counts(status_text: str) -> dict[str, int] | None:
    numbers = re.findall(r"\d+", status_text)
    if len(numbers) < 3:
        return None
    return {"checked": int(numbers[-3]), "matches": int(numbers[-2]),
            "skipped": int(numbers[-1])}


def capture_fileman_search(probe, output: Path, dialog_origin: tuple[int, int],
                            tag: str) -> tuple[object, dict[str, object]]:
    frame = probe.frame(tag)
    dialog_x, dialog_y = dialog_origin
    dialog = client_crop(frame, dialog_x, dialog_y, 900, 560)
    dialog_path = output / f"{tag}.dialog.png"
    dialog.save(dialog_path)

    header = client_crop(frame, dialog_x + 16, dialog_y + 14, 868, 126)
    header_path = output / f"{tag}.header.png"
    header.save(header_path)
    # OCR one label without surrounding input-box borders. Sparse-page OCR
    # can return an empty header even when all Chinese labels are visible.
    label = client_crop(frame, dialog_x + 16, dialog_y + 54, 98, 30)
    label_path = output / f"{tag}.header-label.png"
    label.save(label_path)
    header_text = tesseract_ocr(label, output, f"{tag}-header", psm=7)

    results = client_crop(frame, dialog_x + 16, dialog_y + 154, 868, 300)
    results_path = output / f"{tag}.results.png"
    results.save(results_path)
    results_text = tesseract_ocr(results, output, f"{tag}-results", psm=6)

    selected = client_crop(frame, dialog_x + 16, dialog_y + 462, 868, 28)
    selected_path = output / f"{tag}.selected.png"
    selected.save(selected_path)
    selected_text = tesseract_ocr(selected, output, f"{tag}-selected", psm=7)

    status = client_crop(frame, dialog_x + 16, dialog_y + 496, 868, 26)
    status_path = output / f"{tag}.status.png"
    status.save(status_path)
    status_text = tesseract_ocr(status, output, f"{tag}-status", psm=7)

    evidence = {
        "dialog_screenshot": str(dialog_path),
        "header_screenshot": str(header_path),
        "header_label_screenshot": str(label_path),
        "header_ocr_region": [16, 54, 98, 30],
        "header_ocr_file": str(output / f"{tag}-header.ocr.txt"),
        "header_cjk_count": cjk_count(header_text),
        "results_screenshot": str(results_path),
        "results_ocr_file": str(output / f"{tag}-results.ocr.txt"),
        "results_ocr": results_text.strip(),
        "selected_screenshot": str(selected_path),
        "selected_ocr_file": str(output / f"{tag}-selected.ocr.txt"),
        "selected_ocr": selected_text.strip(),
        "status_screenshot": str(status_path),
        "status_ocr_file": str(output / f"{tag}-status.ocr.txt"),
        "status_ocr": status_text.strip(),
        "status_phase": fileman_search_phase(status_text),
        "status_counts": fileman_search_counts(status_text),
    }
    return frame, evidence


def exercise_fileman_search(probe, geometry: tuple[int, int, int, int],
                            output: Path, failures: list[str]) -> dict[str, object]:
    dialog_origin = (geometry[0] + 40, geometry[1] + 45)
    dialog_open = False
    recursive_enabled = True
    evidence: dict[str, object] = {"performed": True, "passed": False, "checks": {}}
    checks = evidence["checks"]

    def open_dialog_from_main(entry: str) -> None:
        nonlocal dialog_open
        if entry == "ctrl-f":
            probe.click(geometry[0] + 20, geometry[1] + geometry[3] - 22)
            probe.key("ctrl-f")
        elif entry == "address-ctrl-f":
            probe.key("ctrl-l")
            probe.key("ctrl-f")
        elif entry == "toolbar":
            click_client(probe, geometry, geometry[2] - 158, 51)
        else:
            raise ValueError(f"unknown Fileman search entry point: {entry}")
        time.sleep(0.3)
        dialog_open = True

    def enter_query(query: str, *, recursive: bool | None = None,
                    click_search: bool = False) -> None:
        nonlocal recursive_enabled
        if recursive is not None:
            if recursive != recursive_enabled:
                probe.click(dialog_origin[0] + 30, dialog_origin[1] + 108)
                recursive_enabled = recursive
        probe.click(dialog_origin[0] + 350, dialog_origin[1] + 69)
        probe.key("ctrl-a")
        probe.text(query)
        if click_search:
            probe.click(dialog_origin[0] + 828, dialog_origin[1] + 69)
        else:
            probe.key("ret")

    def capture(tag: str, *, settled: bool = True) -> dict[str, object]:
        deadline = time.monotonic() + 10.0
        while True:
            _, captured = capture_fileman_search(probe, output, dialog_origin, tag)
            if not settled or captured["status_phase"] not in (None, "searching"):
                break
            if time.monotonic() >= deadline:
                raise AssertionError(f"search did not settle: {captured['status_ocr']!r}")
            time.sleep(0.15)
        if settled and captured["status_phase"] not in ("complete", "empty"):
            raise AssertionError(f"fixture search did not complete: {captured['status_ocr']!r}")
        checks[tag] = captured
        return captured

    try:
        # The hidden test tree stays out of the existing fixed-row rename/copy
        # scenario. The search dialog inherits Show Hidden=false in this test
        # image, so turn its independent checkbox on explicitly.
        # The rename/copy exercise leaves Fileman in FIXTURE_DIR/roundtrip.
        # Ctrl+L selects the address. Move to its end and erase the ASCII
        # /roundtrip suffix without sending a non-ASCII path through QMP text.
        probe.key("ctrl-l")
        probe.key("end")
        for _ in range(len("/roundtrip")):
            probe.key("backspace")
        probe.key("ret")
        time.sleep(0.4)
        root_frame = probe.frame("app-fileman-search-root-fixture-dir")
        root_address = fileman_address_crop(root_frame, geometry)
        root_address_path = output / "app-fileman-search-root-address.png"
        root_address.save(root_address_path)
        root_address_text = tesseract_ocr(
            root_address, output, "app-fileman-search-root-address", psm=7)
        # Mixed Latin/CJK paths are not reliably transcribed by Tesseract
        # (the real '/home/test/桌面验收目录' was read as '/homey/test/亩面...').
        # Retain this as visual evidence. Finding the unique deep fixture below
        # verifies the navigation; byte-exact paths are covered by host tests.
        checks["initial_root"] = {
            "address_screenshot": str(root_address_path),
            "address_ocr_file": str(output / "app-fileman-search-root-address.ocr.txt"),
            "address_ocr": root_address_text.strip(),
            "address_ocr_is_evidence_only": True,
        }

        open_dialog_from_main("ctrl-f")
        frame = probe.frame("app-fileman-search-open-ctrl-f")
        dialog = client_crop(frame, *dialog_origin, 900, 560)
        dialog.save(output / "app-fileman-search-open-ctrl-f.dialog.png")
        probe.click(dialog_origin[0] + 535, dialog_origin[1] + 108)

        enter_query("deepsearchneedle")
        time.sleep(0.25)
        recursive = capture("app-fileman-search-recursive-hit")
        counts = recursive["status_counts"]
        result_text = str(recursive["results_ocr"])
        if int(recursive["header_cjk_count"]) < 3:
            raise AssertionError("search dialog filename label was not recognized in Chinese")
        if not isinstance(counts, dict) or counts["matches"] != 1:
            raise AssertionError(
                f"recursive search should report one unique fixture match; status={recursive['status_ocr']!r}")
        if (cjk_count(result_text) < 2 or
                not normalized_contains(result_text, "子目录") or
                not normalized_contains(result_text, "deepsearchneedle")):
            raise AssertionError(
                f"recursive result row did not show its full Chinese path: {result_text!r}")
        checks["recursive_hit"] = {"matches": counts["matches"],
                                   "chinese_path_recognized": True}

        enter_query("deepsearchneedle", recursive=False)
        time.sleep(0.25)
        nonrecursive = capture("app-fileman-search-nonrecursive-empty")
        counts = nonrecursive["status_counts"]
        if not isinstance(counts, dict) or counts["matches"] != 0:
            raise AssertionError(
                f"non-recursive search should not descend into the fixture; status={nonrecursive['status_ocr']!r}")
        if (normalized_contains(str(nonrecursive["results_ocr"]), "deepsearchneedle") or
                normalized_contains(str(nonrecursive["selected_ocr"]), "deepsearchneedle")):
            raise AssertionError("non-recursive zero-result search retained a stale result")
        checks["nonrecursive_zero"] = {"matches": 0, "stale_result_cleared": True}

        enter_query("bulkneedle", recursive=True, click_search=True)
        time.sleep(0.35)
        bulk = capture("app-fileman-search-over-64")
        counts = bulk["status_counts"]
        if not isinstance(counts, dict) or counts["matches"] != 80:
            raise AssertionError(
                f"search should retain more than 64 results; status={bulk['status_ocr']!r}")
        checks["over_64"] = {"matches": counts["matches"], "minimum": 65}

        enter_query("deepsearchneedle", recursive=True)
        time.sleep(0.25)
        deep = capture("app-fileman-search-locate-target")
        counts = deep["status_counts"]
        result_text = str(deep["results_ocr"])
        if not isinstance(counts, dict) or counts["matches"] != 1:
            raise AssertionError(
                f"search target should be the only matching result; status={deep['status_ocr']!r}")
        if (cjk_count(result_text) < 2 or
                not normalized_contains(result_text, "deepsearchneedle")):
            raise AssertionError(f"search target row was not visible: {result_text!r}")

        probe.click(dialog_origin[0] + 240, dialog_origin[1] + 164)
        time.sleep(0.15)
        selected_frame = probe.frame("app-fileman-search-selected-target")
        selected_crop = client_crop(
            selected_frame, dialog_origin[0] + 16, dialog_origin[1] + 462, 868, 28)
        selected_crop.save(output / "app-fileman-search-selected-target.png")
        selected_text = tesseract_ocr(
            selected_crop, output, "app-fileman-search-selected-target", psm=7)
        if (cjk_count(selected_text) < 2 or
                not normalized_contains(selected_text, "deepsearchneedle")):
            raise AssertionError(f"selected-path output omitted the UTF-8 search result: {selected_text!r}")
        checks["selected_path"] = {
            "screenshot": str(output / "app-fileman-search-selected-target.png"),
            "ocr_file": str(output / "app-fileman-search-selected-target.ocr.txt"),
            "ocr": selected_text.strip(),
        }

        probe.click(dialog_origin[0] + 258, dialog_origin[1] + 541)
        dialog_open = False
        # The WM title changes before FLTK has necessarily repainted the
        # exposed parent after closing a modal. Wait for the address control.
        address_deadline = time.monotonic() + 5.0
        while True:
            time.sleep(0.2)
            located = probe.frame("app-fileman-search-open-containing-folder")
            address_crop = fileman_address_crop(located, geometry)
            address_text = tesseract_ocr(
                address_crop, output, "app-fileman-search-containing-folder-address", psm=7)
            if (normalized_contains(address_text, ".search-fixture") and
                    normalized_contains(address_text, "子目录")):
                break
            if time.monotonic() >= address_deadline:
                break
        main_client = client_crop(located, *geometry)
        main_path = output / "app-fileman-search-open-containing-folder.client.png"
        main_client.save(main_path)
        main_ocr = tesseract_ocr(
            main_client, output, "app-fileman-search-open-containing-folder", psm=11)
        address_crop.save(output / "app-fileman-search-containing-folder-address.png")
        expected_directory = FIXTURE_DIR + "/.search-fixture/子目录"
        if (not normalized_contains(address_text, ".search-fixture") or
                not normalized_contains(address_text, "子目录")):
            raise AssertionError(
                f"Open Containing Folder navigated to the wrong address: {address_text!r}")
        # Sparse-page OCR omits white text on the selected blue row. Check
        # that row directly, retaining both its raw pixels and grayscale OCR.
        # Exclude the native focus border and the next row's glyphs. Keep
        # the raw crop, normalizing blue/white only for OCR below.
        selected_row = client_crop(main_client, 216, 139, 400, 14)
        selected_row_path = output / "app-fileman-search-located-selected-row.png"
        selected_row.save(selected_row_path)
        selection_pixels = client_crop(main_client, 600, 144, 150, 8)
        colors = selection_pixels.convert("RGB").getcolors(
            maxcolors=selection_pixels.width * selection_pixels.height)
        blue_pixels = sum(count for count, (red, green, blue) in colors
                          if red < 40 and green < 40 and blue > 80)
        selection_fraction = blue_pixels / (selection_pixels.width * selection_pixels.height)
        row_text = selected_row.getchannel("R").point(
            lambda value: 0 if value > 80 else 255)
        row_ocr = tesseract_ocr(
            ImageOps.expand(row_text, 10, 255), output,
            "app-fileman-search-located-selected-row", psm=7, language="eng", scale=3)
        if (selection_fraction < 0.9 or
                not normalized_contains(row_ocr, "deepsearchneedle")):
            raise AssertionError(
                f"Open Containing Folder did not select the matching row: "
                f"blue_fraction={selection_fraction:.3f}, row={row_ocr!r}")
        checks["open_containing_folder"] = {
            "client_screenshot": str(main_path),
            "client_ocr_file": str(output / "app-fileman-search-open-containing-folder.ocr.txt"),
            "address_screenshot": str(output / "app-fileman-search-containing-folder-address.png"),
            "address_ocr_file": str(output / "app-fileman-search-containing-folder-address.ocr.txt"),
            "address_ocr": address_text.strip(),
            "expected_directory": expected_directory,
            "full_address_ocr_is_evidence_only": True,
            "selected_basename_recognized": True,
            "selected_row_screenshot": str(selected_row_path),
            "selected_row_ocr": row_ocr.strip(),
            "selected_row_blue_fraction": selection_fraction,
        }

        open_dialog_from_main("toolbar")
        enter_query("bulkneedle")
        time.sleep(0.35)
        previous_results = capture("app-fileman-search-before-cancel")
        counts = previous_results["status_counts"]
        if not isinstance(counts, dict) or counts["matches"] != 80:
            raise AssertionError(
                f"pre-cancel query did not populate the result browser: status={previous_results['status_ocr']!r}")

        probe.click(dialog_origin[0] + 300, dialog_origin[1] + 29)
        probe.key("ctrl-a")
        probe.text("/usr")
        probe.click(dialog_origin[0] + 350, dialog_origin[1] + 69)
        probe.key("ctrl-a")
        probe.text("cancelsearchfixture")
        probe.key("ret")
        # /usr contains enough entries to keep
        # the timer-driven scan active while the Stop click is delivered.
        probe.click(dialog_origin[0] + 684, dialog_origin[1] + 541)
        time.sleep(0.2)
        stopped = capture("app-fileman-search-canceled", settled=False)
        counts = stopped["status_counts"]
        if stopped["status_phase"] != "stopped":
            raise AssertionError(
                f"Stop did not report a canceled search (a completed scan is not a pass): "
                f"status={stopped['status_ocr']!r}")
        if not isinstance(counts, dict) or counts["checked"] <= 0:
            raise AssertionError(
                f"canceled search did not record any scan progress: status={stopped['status_ocr']!r}")
        if (normalized_contains(str(stopped["results_ocr"]), "bulkneedle") or
                normalized_contains(str(stopped["selected_ocr"]), "bulkneedle")):
            raise AssertionError("starting a new query retained results from the previous search")
        checks["cancel"] = {"status_phase": stopped["status_phase"],
                            "checked_before_stop": counts["checked"],
                            "old_results_cleared": True}

        probe.click(dialog_origin[0] + 300, dialog_origin[1] + 29)
        probe.key("ctrl-a")
        probe.text("/tmp")
        probe.click(dialog_origin[0] + 350, dialog_origin[1] + 69)
        probe.key("ctrl-a")
        probe.text("freshquery")
        probe.click(dialog_origin[0] + 828, dialog_origin[1] + 69)
        time.sleep(0.35)
        fresh = capture("app-fileman-search-after-cancel")
        counts = fresh["status_counts"]
        if (fresh["status_phase"] not in ("complete", "empty") or
                not isinstance(counts, dict) or counts["matches"] != 0):
            raise AssertionError(
                f"a fresh query did not complete after cancellation: status={fresh['status_ocr']!r}")
        if normalized_contains(str(fresh["results_ocr"]), "bulkneedle"):
            raise AssertionError("fresh query displayed stale pre-cancel results")
        checks["after_cancel_query"] = {"matches": 0, "old_results_cleared": True,
                                        "search_button_responsive": True}

        probe.click(dialog_origin[0] + 818, dialog_origin[1] + 541)
        dialog_open = False
        time.sleep(0.25)
        probe.key("ctrl-f")
        time.sleep(0.25)
        dialog_open = True
        responsive_frame, responsive = capture_fileman_search(
            probe, output, dialog_origin, "app-fileman-search-controls-responsive")
        if int(responsive["header_cjk_count"]) < 3:
            raise AssertionError("Ctrl+F did not reopen the search dialog after Close")
        responsive_path = responsive["dialog_screenshot"]
        probe.key("esc")
        time.sleep(0.25)
        closed_frame = probe.frame("app-fileman-search-escape-closed")
        if frame_distance(responsive_frame, closed_frame) < 0.01:
            raise AssertionError("Escape did not close the responsive search dialog")
        dialog_open = False
        checks["controls_responsive"] = {"reopened_via_ctrl_f": True,
                                         "screenshot": str(responsive_path)}

        # Open a directory result with Enter, exercising the stored full path
        # instead of only the separate Open Containing Folder action.
        open_dialog_from_main("address-ctrl-f")
        probe.click(dialog_origin[0] + 300, dialog_origin[1] + 29)
        probe.key("ctrl-a")
        probe.text("/home/test")
        probe.click(dialog_origin[0] + 535, dialog_origin[1] + 108)
        enter_query("search-fixture")
        time.sleep(0.35)
        directory_result = capture("app-fileman-search-open-directory-result")
        counts = directory_result["status_counts"]
        if (directory_result["status_phase"] != "complete" or
                not isinstance(counts, dict) or counts["matches"] != 1):
            raise AssertionError(
                f"directory query did not finish with one match: {directory_result['status_ocr']!r}")
        probe.click(dialog_origin[0] + 240, dialog_origin[1] + 164)
        probe.key("ret")
        dialog_open = False
        time.sleep(0.35)
        opened_directory = probe.frame("app-fileman-search-directory-opened-with-enter")
        opened_address = fileman_address_crop(opened_directory, geometry)
        opened_address.save(output / "app-fileman-search-directory-opened-address.png")
        opened_address_text = tesseract_ocr(
            opened_address, output, "app-fileman-search-directory-opened-address", psm=7)
        if (not normalized_contains(opened_address_text, ".search-fixture") or
                normalized_contains(opened_address_text, "子目录")):
            raise AssertionError(
                f"Enter did not open the directory result: {opened_address_text!r}")
        checks["open_directory_with_enter"] = {
            "expected_path": FIXTURE_DIR + "/.search-fixture",
            "address_ocr": opened_address_text.strip(),
            "address_screenshot": str(output / "app-fileman-search-directory-opened-address.png"),
        }
        evidence["passed"] = True
    except Exception as error:
        failures.append(f"fileman: search GUI regression failed: {error}")
        evidence["error"] = str(error)
        if dialog_open:
            try:
                probe.key("esc")
                time.sleep(0.2)
            except Exception:
                pass
    return evidence


def run_guest_verifier(probe, serial: Path, process, helper: str,
                       marker: str) -> str:
    offset = len(serial.read_text(errors="replace"))
    xorg.st_marker(probe, serial, process, marker, helper)
    text = wait_marker(serial, marker, process, offset, 20)
    output = text[offset:]
    result_line = next((line for line in output.splitlines() if marker in line), "")
    if not result_line.endswith("PASS"):
        raise AssertionError(f"guest verification helper failed: {result_line!r}")
    return result_line


def exercise_fileman(probe, geometry: tuple[int, int, int, int],
                     serial: Path, process, output: Path, failures: list[str],
                     fileman_pid: int) -> dict[str, object]:
    # Save the UTF-8 address through the native clipboard; QMP key typing is
    # ASCII-only and must not be used to reconstruct the Chinese fixture path.
    probe.key("ctrl-l")
    probe.key("ctrl-c")
    # A selected Hold Browser row must still receive its double-click action.
    # Check real child text below the root, not only a changed selection color.
    def tree_children(tag):
        probe.move(geometry[0] + geometry[2] - 24,
                   geometry[1] + geometry[3] - 12)
        time.sleep(0.2)
        frame = probe.frame(tag)
        crop = client_crop(frame, geometry[0] + 20, geometry[1] + 154, 190, 90)
        crop.save(output / f"{tag}.children.png")
        tesseract_ocr(crop, output, tag, psm=6)
        # The interior below the root is plain white when collapsed. Checking
        # glyph pixels also avoids OCR dropping the first line at a crop edge.
        ink = sum(value < 100 for value in crop.convert("L").tobytes())
        return ink / (crop.width * crop.height)

    def double_click_root():
        probe.move(geometry[0] + 55, geometry[1] + 146)
        for _ in range(2):
            probe.button(True)
            time.sleep(0.06)
            probe.button(False)
            time.sleep(0.08)

    tree_before = tree_children("app-fileman-tree-before")
    if tree_before < 0.003:
        raise AssertionError("folder tree did not initially show root children")
    double_click_root()
    tree_collapsed = tree_children("app-fileman-tree-collapsed")
    if tree_collapsed > 0.0005:
        raise AssertionError("double-click did not collapse the selected root folder")
    time.sleep(0.4)
    double_click_root()
    tree_expanded = tree_children("app-fileman-tree-expanded")
    if tree_expanded < 0.003:
        raise AssertionError("double-click did not restore root folder children")
    probe.key("ctrl-l")
    probe.key("ctrl-v")
    probe.key("ret")
    time.sleep(0.3)

    click_client(probe, geometry, 66, 88)
    probe.key("ctrl-a")
    probe.text("roundtrip")
    probe.key("ret")
    time.sleep(0.4)

    # Double-click the new directory, then navigate back. This checks the
    # visible open hint against real navigation rather than only labels.
    probe.move(geometry[0] + 500, geometry[1] + 148)
    for _ in range(2):
        probe.button(True)
        time.sleep(0.06)
        probe.button(False)
        time.sleep(0.08)
    time.sleep(0.4)
    opened_folder = probe.frame("app-fileman-double-click-directory")
    opened_address = fileman_address_crop(opened_folder, geometry)
    opened_text = tesseract_ocr(opened_address, output,
                              "app-fileman-double-click-directory-address", psm=7)
    if not normalized_contains(opened_text, "roundtrip"):
        raise AssertionError(f"double-click did not open the new folder: {opened_text!r}")
    probe.key("alt-up")
    time.sleep(0.3)

    # Directories sort before files, so the Chinese fixture is the second row.
    click_client(probe, geometry, 500, 164)
    probe.key("f2")
    probe.key("end")
    for _ in range(4):
        probe.key("left")
    probe.text("_renamed")
    probe.key("ret")
    time.sleep(0.3)

    click_client(probe, geometry, 500, 164)
    probe.key("ctrl-c")
    probe.key("ctrl-l")
    probe.key("end")
    probe.text("/roundtrip")
    probe.key("ret")
    time.sleep(0.3)
    click_client(probe, geometry, 500, 148)
    probe.key("ctrl-v")
    time.sleep(0.8)

    search_result = exercise_fileman_search(probe, geometry, output, failures)

    # Navigate to the exact allowlisted installer ELF through fileman.  Its
    # address callback is directory-only, so select the ELF in its parent.
    installer_path = "/usr/lib/reliefos/apps/installer/installer.elf"
    installer_dir = "/usr/lib/reliefos/apps/installer"
    probe.key("ctrl-l")
    probe.key("ctrl-a")
    probe.text(installer_dir)
    probe.key("ret")
    time.sleep(0.4)
    before_child = probe.frame("app-fileman-before-child-installer")
    open_offset = len(serial.read_text(errors="replace"))
    # installer.bmp sorts before installer.elf in the staged directory.
    click_client(probe, geometry, 500, 164)
    probe.key("ret")

    child_pid = None
    child_started = False
    child_result: dict[str, object] = {"performed": False, "path": installer_path}
    try:
        # The kernel exec trace is written immediately. fileman writes its
        # equivalent C stdio open-path line only when its process exits.
        exec_marker = f"path={installer_path} pty="
        launched = wait_marker(serial, exec_marker, process, open_offset, 20)
        match = re.search(
            r"exec pid=(\d+) path=" + re.escape(installer_path) + r" pty=",
            launched[open_offset:])
        if not match:
            raise AssertionError(
                f"kernel exec trace omitted the exact installer PID: {launched[open_offset:]!r}")
        child_pid = int(match.group(1))
        if child_pid <= 0 or child_pid == fileman_pid:
            raise AssertionError(
                f"installer did not receive an independent PID: {child_pid}")
        wait_marker(serial, READY_MARKERS["installer"], process, open_offset, 25)
        child_started = True

        deadline = time.monotonic() + 10
        child_frame = None
        visible_page = None
        while time.monotonic() < deadline:
            candidate = probe.frame("app-fileman-child-installer")
            visible_page = probe.visible_page(candidate)
            if visible_page == 0:
                child_frame = candidate
                break
            time.sleep(0.2)
        if child_frame is None:
            raise AssertionError(
                f"installer PID {child_pid} reported ready but no page-0 window was visible")
        child_distance = frame_distance(before_child, child_frame)
        if child_distance < 0.004:
            raise AssertionError(
                f"installer PID {child_pid} did not create a visible independent window")
        child_ocr = tesseract_ocr(
            child_frame, output, "app-fileman-child-installer", psm=11)

        # Click the inert brand area to focus the child window, then close it.
        sidebar_left = getattr(probe, "sidebar_left", None)
        sidebar_top = getattr(probe, "sidebar_top", None)
        if sidebar_left is None or sidebar_top is None:
            raise AssertionError("could not identify the installer window for safe close")
        probe.click(sidebar_left + 32, sidebar_top + 24)
        probe.key("alt-f4")
        closed_frame = None
        close_deadline = time.monotonic() + 8
        while time.monotonic() < close_deadline:
            candidate = probe.frame("app-fileman-after-child-installer-close")
            if probe.visible_page(candidate) is None:
                closed_frame = candidate
                break
            time.sleep(0.2)
        if closed_frame is None:
            # Retry once after re-focusing the known inert area; this avoids
            # accidentally sending the later fileman close to its child.
            probe.click(sidebar_left + 32, sidebar_top + 24)
            probe.key("alt-f4")
            retry_deadline = time.monotonic() + 5
            while time.monotonic() < retry_deadline:
                candidate = probe.frame("app-fileman-after-child-installer-close")
                if probe.visible_page(candidate) is None:
                    closed_frame = candidate
                    break
                time.sleep(0.2)
        if closed_frame is None:
            raise AssertionError(f"installer PID {child_pid} did not close on Alt+F4")
        child_started = False

        fileman_client = client_crop(closed_frame, *geometry)
        fileman_client.save(output / "app-fileman-after-installer-parent.client.png")
        before_client = client_crop(before_child, *geometry)
        parent_client_distance = frame_distance(before_client, fileman_client)
        address = client_crop(fileman_client, 204, 36, geometry[2] - 416, 30)
        address.save(output / "app-fileman-after-installer-address.png")
        address_text = tesseract_ocr(
            address, output, "app-fileman-after-installer-address", psm=7)
        if parent_client_distance >= 0.06:
            raise AssertionError(
                "fileman client did not return to its pre-launch contents after the "
                f"installer closed (visual distance={parent_client_distance:.6f})")
        if f"[fltk-runner] EXIT app=fileman pid={fileman_pid} " in serial.read_text(errors="replace"):
            raise AssertionError("fileman exited when its installer child closed")
        child_result = {
            "performed": True, "path": installer_path,
            "pid": child_pid, "fileman_pid": fileman_pid,
            "independent_pid": child_pid != fileman_pid,
            "open_log_expected": f"[fileman.elf] open path={installer_path} pid={child_pid}",
            "ready_marker": READY_MARKERS["installer"],
            "visible_page": visible_page,
            "window_visual_distance": round(child_distance, 6),
            "window_screenshot": str(output / "app-fileman-child-installer.png"),
            "ocr_file": str(output / "app-fileman-child-installer.ocr.txt"),
            "fileman_parent_client_screenshot": str(
                output / "app-fileman-after-installer-parent.client.png"),
            "fileman_parent_client_visual_distance": round(
                parent_client_distance, 6),
            "closed_with_alt_f4": True,
            "fileman_still_open": True,
            "fileman_address_ocr": address_text.strip(),
            "fileman_address_ocr_is_evidence_only": True,
            "disk_selected": False, "install_started": False,
        }
    except (AssertionError, RuntimeError) as error:
        failures.append(f"fileman: installer child GUI check failed: {error}")
        child_result["error"] = str(error)
        if child_started:
            sidebar_left = getattr(probe, "sidebar_left", None)
            sidebar_top = getattr(probe, "sidebar_top", None)
            if sidebar_left is not None and sidebar_top is not None:
                probe.click(sidebar_left + 32, sidebar_top + 24)
            else:
                probe.click(probe.width // 2, probe.height // 2)
            probe.key("alt-f4")
            time.sleep(0.4)

    return {"performed": True,
            "actions": ["double-click folder tree collapse and expand", "new ASCII folder", "double-click directory and Alt+Up", "rename UTF-8 filename",
                        "copy", "navigate by address", "paste",
                        "recursive Chinese filename search", "non-recursive zero-result search",
                        "more than 64 search results", "Open Containing Folder",
                        "stop and restart a search",
                        "open allowlisted installer ELF in GUI", "close child; retain fileman"],
            "search": search_result,
            "folder_tree_double_click": True,
            "folder_tree_ink_fractions": [tree_before, tree_collapsed, tree_expanded],
            "installer_child": child_result,
            "verification_helper": FILEMAN_VERIFY_GUEST}


def exercise_paint(probe, output: Path, failures: list[str],
                   geometry: tuple[int, int, int, int]) -> dict[str, object]:
    window_x, window_y, window_w, window_h = geometry
    click_client(probe, geometry, 392, 72)
    click_client(probe, geometry, 22, 112)
    time.sleep(0.3)
    probe.move(window_x + 800, window_y + 500)
    drawn = probe.frame("app-paint-after-draw")
    painted_rgb = drawn.getpixel((window_x + 22, window_y + 112))
    if not (painted_rgb[0] > 200 and painted_rgb[1] < 80 and painted_rgb[2] < 80):
        failures.append(f"paint canvas did not receive red brush: {painted_rgb!r}")

    probe.key("alt-f4")
    time.sleep(0.4)
    guarded = probe.frame("app-paint-unsaved-guard")
    guard_distance = xorg.frame_distance(drawn, guarded)
    if guard_distance < 0.003:
        failures.append("Alt+F4 did not display the unsaved-changes guard")
    guarded_pixel = guarded.getpixel((window_x + 22, window_y + 112))
    if not (guarded_pixel[0] > 200 and guarded_pixel[1] < 80 and guarded_pixel[2] < 80):
        failures.append("unsaved guard obscured the changed canvas pixel")

    probe.key("esc")
    time.sleep(0.4)
    canceled = probe.frame("app-paint-guard-canceled")
    cancel_distance = xorg.frame_distance(drawn, canceled)
    if cancel_distance > 0.025:
        failures.append(
            f"Escape did not cancel the close guard and restore the canvas ({cancel_distance:.4f})")
    canceled_pixel = canceled.getpixel((window_x + 22, window_y + 112))
    if not (canceled_pixel[0] > 200 and canceled_pixel[1] < 80 and canceled_pixel[2] < 80):
        failures.append("paint window did not remain open after canceling close")

    probe.key("ctrl-a")
    chooser_frame = None
    chooser_text = ""
    chooser_ready = False
    chooser_deadline = time.monotonic() + 8
    while time.monotonic() < chooser_deadline:
        candidate = probe.frame("app-paint-save-chooser")
        candidate_text = tesseract_ocr(
            candidate, output, "app-paint-save-chooser", psm=11)
        label_visible = normalized_contains(candidate_text, "文件名")
        basename_visible = normalized_contains(candidate_text, "扫雷矿石.bmp")
        directory_visible = normalized_contains(candidate_text, "桌面验收目录")
        if label_visible and basename_visible and directory_visible:
            chooser_frame = candidate
            chooser_text = candidate_text
            chooser_ready = True
            break
        chooser_frame = candidate
        chooser_text = candidate_text
        time.sleep(0.3)
    chooser_ocr_path = str(output / "app-paint-save-chooser.ocr.txt")
    chooser_frame.save(output / "app-paint-save-chooser.png")
    if not chooser_ready:
        failures.append(
            "paint Save As chooser OCR missed filename label, Chinese basename, "
            f"or directory: {chooser_text!r}")
    probe.key("esc")
    chooser_cancelled = probe.frame("app-paint-save-chooser-canceled")
    chooser_cancel_distance = xorg.frame_distance(canceled, chooser_cancelled)
    chooser_cancelled.save(output / "app-paint-save-chooser-canceled.png")
    if chooser_cancel_distance > 0.025:
        failures.append(
            f"Escape did not cancel the Save As chooser ({chooser_cancel_distance:.4f})")

    click_client(probe, geometry, 312, 29)
    time.sleep(0.4)
    probe.move(window_x + 800, window_y + 500)
    saved = probe.frame("app-paint-saved")
    status = client_crop(saved, window_x + 12, window_y + 660, 916, 28)
    status.save(output / "app-paint-save-status.png")
    status_text = tesseract_ocr(status, output, "app-paint-save-status", psm=7)
    if not normalized_contains(status_text, "图片已保存"):
        failures.append(f"paint save status was not recognized: {status_text!r}")

    probe.key("alt-f4")
    interaction = {
        "performed": True,
        "actions": ["select red", "draw", "Alt+F4 unsaved guard", "Escape cancel",
                    "Ctrl+A Save As chooser and OCR", "Escape cancel chooser",
                    "save", "Alt+F4 close"],
        "canvas_pixel_rgb": painted_rgb,
        "guard_visual_distance": round(guard_distance, 6),
        "cancel_visual_distance": round(cancel_distance, 6),
        "chooser_ocr_file": chooser_ocr_path,
        "chooser_label_visible": normalized_contains(chooser_text, "文件名"),
        "chooser_chinese_basename_visible": normalized_contains(chooser_text, "扫雷矿石.bmp"),
        "chooser_chinese_directory_visible": normalized_contains(chooser_text, "桌面验收目录"),
        "chooser_cancel_visual_distance": round(chooser_cancel_distance, 6),
        "save_status_ocr": status_text.strip(),
        "verification_helper": PAINT_VERIFY_GUEST,
    }
    return interaction


def exercise_installer_readonly(probe) -> dict[str, object]:
    page_frames = []
    frame = probe.frame("app-installer-page-0-before-language")
    if probe.visible_page(frame) != 0:
        raise AssertionError("installer did not start on language page 0")
    click = probe.language_option_point(chinese=True)
    probe.click(*click)
    frame = probe.frame("app-installer-page-0-chinese")
    if probe.visible_page(frame) != 0:
        raise AssertionError("selecting Chinese left installer language page 0")
    page_frames.append({"page": 0, "screenshot": "app-installer-page-0-chinese.png"})

    previous = frame
    for expected in range(1, 5):
        probe.click(*probe.footer_point())
        deadline = time.monotonic() + 6
        frame = None
        while time.monotonic() < deadline:
            current = probe.frame(f"app-installer-page-{expected}")
            page = probe.visible_page(current)
            if page == expected:
                frame = current
                break
            if page is not None and page != expected - 1:
                raise AssertionError(
                    f"installer Next reached page {page}, expected {expected}")
            time.sleep(0.15)
        if frame is None:
            raise AssertionError(f"installer Next did not reach page {expected}")
        distance = xorg.frame_distance(previous, frame)
        if distance < 0.0005:
            raise AssertionError(f"installer page {expected} did not visibly change")
        page_frames.append({"page": expected,
                            "screenshot": f"app-installer-page-{expected}.png",
                            "visual_distance": round(distance, 6)})
        previous = frame

    # Page 4 is the disk page. Stop here and move back without selecting a disk.
    for expected in range(3, -1, -1):
        probe.click(*probe.footer_point(previous=True))
        deadline = time.monotonic() + 6
        frame = None
        while time.monotonic() < deadline:
            current = probe.frame(f"app-installer-page-{expected}-back")
            page = probe.visible_page(current)
            if page == expected:
                frame = current
                break
            if page is not None and page != expected + 1:
                raise AssertionError(
                    f"installer Previous reached page {page}, expected {expected}")
            time.sleep(0.15)
        if frame is None:
            raise AssertionError(f"installer Previous did not return to page {expected}")
        distance = xorg.frame_distance(previous, frame)
        if distance < 0.0005:
            raise AssertionError(f"installer page {expected} did not visibly change on Back")
        page_frames.append({"page": expected,
                            "screenshot": f"app-installer-page-{expected}-back.png",
                            "visual_distance": round(distance, 6)})
        previous = frame

    return {"performed": True, "read_only": True, "pages": page_frames,
            "disk_selected": False, "install_started": False}


def run_one_app(app: str, probe, serial: Path, process, baseline,
                output: Path, failures: list[str]) -> dict[str, object]:
    offset = len(serial.read_text(errors="replace"))
    command = f"{APP_RUNNER_GUEST} {app}"
    xorg.st_marker(probe, serial, process, f"[fltk-runner] START app={app}", command)
    text = wait_marker(serial, f"[fltk-runner] START app={app}", process, offset, 20)
    start_line = next((line for line in text[offset:].splitlines()
                       if f"[fltk-runner] START app={app} " in line), "")
    pid_match = re.search(r"\bpid=(\d+)", start_line)
    if not pid_match:
        raise AssertionError(f"runner start marker omitted PID for {app}: {start_line!r}")
    pid = int(pid_match.group(1))
    ready = READY_MARKERS.get(app)
    if app == "doom":
        wait_marker(serial, ready, process, offset, 45)
        time.sleep(0.7)
        if f"[fltk-runner] EXIT app=doom pid={pid} " in serial.read_text(errors="replace"):
            raise AssertionError("DOOM exited before its first game-frame capture")
    elif ready:
        wait_marker(serial, ready, process, offset, 35)

    window_x = window_y = 0
    window_w, window_h = probe.width, probe.height
    if app != "doom":
        geometry_marker = f"FLTK_APP_GEOMETRY pid={pid} "
        text = wait_marker(serial, geometry_marker, process, offset, 15)
        geometry = re.search(re.escape(geometry_marker) +
                             r"x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", text[offset:])
        if not geometry:
            raise AssertionError(f"{app}: missing mapped client geometry")
        window_x, window_y, window_w, window_h = map(int, geometry.groups())

    if app in ("fileman", "taskmgr"):
        # Newly added tooltips are useful UI, but a pointer left on a toolbar
        # button can obscure its label during the startup screenshot/OCR.
        probe.move(window_x + window_w - 24, window_y + window_h - 12)
        time.sleep(0.2)

    expected = CHINESE_EXPECTATIONS.get(app)
    anchor_box = CLIENT_OCR_ANCHORS.get(app)
    anchor_tag = f"app-{app}-client-anchor" if anchor_box else None
    expected_label_ocr_file = None
    if anchor_tag:
        expected_label_ocr_file = str(output / f"{anchor_tag}.ocr.txt")

    gallery_app = app in ("leonmmcoset", "xiaobai")
    gallery_source_path = None
    gallery_expected_frame = None
    gallery_image_rect = None
    gallery_expected_screenshot = None
    client_png_match_fraction = 0.0
    client_png_mean_max_error = 0.0
    if gallery_app:
        gallery_source_path, gallery_expected_frame, gallery_image_rect = expected_png_client(
            app, window_w, window_h)
        gallery_expected_screenshot = output / f"app-{app}-expected-client.png"
        gallery_expected_frame.save(gallery_expected_screenshot)

    wait_for_client = bool(expected) or gallery_app
    deadline = time.monotonic() + (20 if wait_for_client else 0)
    capture_attempts = 0
    expected_text = ""
    screen_text = ""
    while True:
        capture_attempts += 1
        frame = probe.frame(f"app-{app}")
        client_frame = frame if app == "doom" else client_crop(
            frame, window_x, window_y, window_w, window_h)
        if expected:
            if anchor_box:
                label = client_crop(frame, window_x + anchor_box[0],
                                    window_y + anchor_box[1],
                                    anchor_box[2], anchor_box[3])
                label.save(output / f"{anchor_tag}.png")
                ocr_label = label
                if app in CLIENT_OCR_TEXT_INSETS:
                    inset = CLIENT_OCR_TEXT_INSETS[app]
                    ocr_label = client_crop(label, *inset)
                    ocr_label.save(output / f"{anchor_tag}-ocr-input.png")
                expected_text = tesseract_ocr(
                    ocr_label, output, anchor_tag, psm=7)
            else:
                screen_text = tesseract_ocr(client_frame, output, f"app-{app}")
                expected_text = screen_text
            client_ready = normalized_contains(expected_text, expected)
        elif gallery_app:
            client_png_match_fraction, client_png_mean_max_error = png_content_match(
                client_frame, gallery_expected_frame, gallery_image_rect,
                PNG_PIXEL_CHANNEL_TOLERANCE)
            client_ready = client_png_match_fraction >= PNG_CONTENT_MATCH_MIN_FRACTION
        else:
            client_ready = True

        if client_ready or time.monotonic() >= deadline:
            break
        time.sleep(0.4)

    if not screen_text:
        screen_text = tesseract_ocr(client_frame, output, f"app-{app}")
    if app != "doom":
        client_frame.save(output / f"app-{app}.client.png")
    distance = frame_distance(baseline, frame)
    if distance < 0.004:
        failures.append(f"{app}: screenshot did not materially change from desktop")
    if expected and not normalized_contains(expected_text, expected):
        failures.append(
            f"{app}: OCR missed expected client label {expected!r} after "
            f"{capture_attempts} capture(s): {expected_text!r}")
    if gallery_app and not client_ready:
        failures.append(
            f"{app}: client pixels did not match the decoded PNG after {capture_attempts} "
            f"capture(s) (within {PNG_PIXEL_CHANNEL_TOLERANCE}/255 match="
            f"{client_png_match_fraction:.6f}, required>="
            f"{PNG_CONTENT_MATCH_MIN_FRACTION:.3f}, mean max-channel error="
            f"{client_png_mean_max_error:.3f}, source={gallery_source_path})")

    interaction: dict[str, object] = {"performed": False}
    resize_evidence = None
    if app in ("fileman", "taskmgr"):
        resized, resize_evidence = exercise_window_resizing(
            app, probe, (window_x, window_y, window_w, window_h),
            serial, process, output, pid)
        window_x, window_y, window_w, window_h = resized
    close_sent = False
    if app == "calc":
        for key in ("3", "shift-equal", "4", "ret"):
            probe.key(key)
        operated = probe.frame("app-calc-after-3-plus-4")
        # Exclude number buttons and terminal text from calculation evidence.
        display = operated.crop((window_x + 166, window_y + 122,
                                 window_x + 740, window_y + 166))
        display.save(output / "app-calc-result.png")
        operated_text = tesseract_ocr(display, output, "app-calc-result", psm=7)
        interaction = {"performed": True, "action": "keyboard 3 + 4 =",
                       "ocr_file": str(output / "app-calc-result.ocr.txt"),
                       "result_digit_visible": normalized_contains(operated_text, "7")}
        if not normalized_contains(operated_text, "7"):
            failures.append("calc: OCR did not show the result of 3 + 4")
    elif app == "fileman":
        interaction = exercise_fileman(
            probe, (window_x, window_y, window_w, window_h),
            serial, process, output, failures, pid)
    elif app == "taskmgr":
        interaction = exercise_taskmgr(
            probe, (window_x, window_y, window_w, window_h),
            serial, process, output, failures, pid, sys.modules[__name__])
    elif app == "paint":
        interaction = exercise_paint(
            probe, output, failures, (window_x, window_y, window_w, window_h))
        close_sent = True
    elif app == "installer":
        interaction = exercise_installer_readonly(probe)

    if resize_evidence is not None:
        interaction["resizing"] = resize_evidence

    close_mode = "timeout_sigterm" if app == "doom" else "window_manager_alt_f4"
    if app != "doom" and not close_sent:
        if app == "fileman":
            # The child installer may have taken focus before it closed. Focus
            # fileman's read-only status strip explicitly before sending Alt+F4.
            probe.click(window_x + 20, window_y + window_h - 22)
        probe.key("alt-f4")
    if close_sent:
        close_mode = "interaction_alt_f4"
    exit_marker = f"[fltk-runner] EXIT app={app} pid={pid} status="
    try:
        final = wait_marker(serial, exit_marker, process, offset,
                            DOOM_SECONDS + 20 if app == "doom" else 20)
    except (AssertionError, RuntimeError) as error:
        failures.append(f"{app}: window did not close and emit PID/exit marker: {error}")
        return {"pid": pid, "ready_marker": ready, "screenshot": str(output / f"app-{app}.png"),
                "ocr_file": str(output / f"app-{app}.ocr.txt"),
                "ocr_cjk_count": cjk_count(screen_text), "expected_chinese": expected,
                "visual_distance": distance, "close_mode": close_mode,
                "capture_attempts": capture_attempts,
                "exit_status": None, "interaction": interaction}
    exit_line = next((line for line in final.splitlines()
                      if f"[fltk-runner] EXIT app={app} pid={pid} " in line), "")
    exit_match = re.search(r"\bstatus=(\d+)", exit_line)
    status = int(exit_match.group(1)) if exit_match else None
    if app == "fileman":
        child = interaction.get("installer_child")
        if isinstance(child, dict) and child.get("pid"):
            expected_open = child.get("open_log_expected")
            exact_open_pattern = (r"\[fileman\.elf\] open path=" +
                                  re.escape("/usr/lib/reliefos/apps/installer/installer.elf") +
                                  r" pid=(\d+)")
            open_match = re.search(exact_open_pattern, final[offset:])
            if not open_match:
                failures.append(
                    "fileman: buffered exact installer open-path log was absent after parent exit")
            elif int(open_match.group(1)) != int(child["pid"]):
                failures.append(
                    f"fileman: open-path log PID {open_match.group(1)} did not match "
                    f"kernel exec PID {child['pid']}")
            else:
                child["open_log_verified_after_parent_exit"] = True
                child["open_log_expected"] = expected_open
    acceptable_status = status == 0 or (app == "doom" and status in (124, 137, 143))
    if not acceptable_status:
        failures.append(f"{app}: unexpected exit status {status!r}: {exit_line!r}")
    if app == "fileman":
        try:
            verifier_line = run_guest_verifier(
                probe, serial, process, FILEMAN_VERIFY_GUEST, "FLTK_FILEMAN_VERIFY")
            interaction["verification"] = verifier_line
        except (AssertionError, RuntimeError) as error:
            failures.append(f"fileman: round-trip filesystem verification failed: {error}")
    elif app == "paint":
        try:
            verifier_line = run_guest_verifier(
                probe, serial, process, PAINT_VERIFY_GUEST, "FLTK_PAINT_VERIFY")
            interaction["verification"] = verifier_line
        except (AssertionError, RuntimeError) as error:
            failures.append(f"paint: saved-image verification failed: {error}")
    return {"pid": pid, "ready_marker": ready or "visible capture interval",
            "screenshot": str(output / f"app-{app}.png"),
            "client_screenshot": (str(output / f"app-{app}.client.png")
                                  if app != "doom" else str(output / f"app-{app}.png")),
            "ocr_file": str(output / f"app-{app}.ocr.txt"),
            "expected_label_ocr_file": expected_label_ocr_file,
            "ocr_cjk_count": cjk_count(screen_text), "expected_chinese": expected,
            "expected_chinese_recognized": bool(expected and normalized_contains(expected_text, expected)),
            "visual_distance": round(distance, 6), "close_mode": close_mode,
            "capture_attempts": capture_attempts,
            "client_png_source": (str(gallery_source_path)
                                  if gallery_app else None),
            "client_png_source_sha256": (sha256(gallery_source_path)
                                         if gallery_app else None),
            "client_expected_screenshot": (str(gallery_expected_screenshot)
                                           if gallery_expected_screenshot else None),
            "client_png_image_rect": (list(gallery_image_rect)
                                      if gallery_image_rect else None),
            "client_png_pixel_tolerance": (PNG_PIXEL_CHANNEL_TOLERANCE
                                           if gallery_app else None),
            "client_png_match_fraction": (round(client_png_match_fraction, 6)
                                          if gallery_app else None),
            "client_png_mean_max_channel_error": (round(client_png_mean_max_error, 3)
                                                   if gallery_app else None),
            "client_geometry": [window_x, window_y, window_w, window_h],
            "exit_status": status, "interaction": interaction}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE,
                        help="old xorg-vmware-debug raw image; it is never modified")
    parser.add_argument("--kernel", type=Path, default=DEFAULT_KERNEL,
                        help="current standalone kernel.sys injected into the private ESP")
    parser.add_argument("--sysroot", type=Path, default=DEFAULT_SYSROOT,
                        help="pinned x86_64-musl sysroot used to build the GUI probe")
    parser.add_argument("--probe-elf", type=Path,
                        help="use a prebuilt target probe instead of compiling it here")
    parser.add_argument("--skip-input-probe", action="store_true",
                        help="skip the unchanged shared UTF-8 widget probe for focused app checks")
    parser.add_argument("--apps", nargs="+", choices=APP_NAMES, default=list(APP_NAMES),
                        help="apps to launch; payload injection still includes all 12 current ELFs")
    parser.add_argument("--cxx", default="clang++", help="configured target C++ compiler")
    parser.add_argument("--ld", default="ld.lld", help="configured target linker")
    parser.add_argument("--output", type=Path,
                        help="new evidence directory under build/fltk-migration")
    parser.add_argument("--boot-timeout", type=float, default=180,
                        help="maximum seconds to reach the graphical greeter")
    return parser.parse_args()


def create_output(requested: Path | None) -> Path:
    build_root = (ROOT / "build/fltk-migration").resolve()
    build_root.mkdir(parents=True, exist_ok=True)
    if requested:
        output = requested.resolve()
        if not output.is_relative_to(build_root) or output.exists():
            raise SystemExit("--output must name a new directory below build/fltk-migration/")
        output.mkdir(parents=True)
        return output
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return Path(tempfile.mkdtemp(prefix=f"{stamp}-{secrets.token_hex(3)}-", dir=build_root))


def main() -> int:
    args = parse_args()
    temporary_root = ROOT / "out/tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(temporary_root)
    tempfile.tempdir = str(temporary_root)
    output = create_output(args.output)
    image = args.image.resolve()
    kernel = args.kernel.resolve()
    sysroot = args.sysroot.resolve()
    for path, label in (
        (image, "input raw image"), (kernel, "current kernel"),
        (sysroot / "include/stdio.h", "musl headers"),
        (sysroot / "lib/libc.so", "musl runtime"),
        (sysroot / "lib/libmimalloc.so.3", "musl mimalloc runtime"),
        (X11_ROOT / "usr/include/FL/Fl.H", "staged FLTK headers"),
        (X11_ROOT / "usr/lib/libfltk.so", "staged target FLTK library"),
        (OUT / "generated/nls/zh_CN/LC_MESSAGES/leonos.mo", "current zh_CN catalog"),
    ):
        if not path.is_file():
            raise SystemExit(f"missing {label}: {path}")
    languages = subprocess.check_output(["tesseract", "--list-langs"], text=True,
                                        stderr=subprocess.STDOUT)
    if "chi_sim" not in languages or "eng" not in languages:
        raise SystemExit("Tesseract must have both chi_sim and eng trained data installed")

    source_image_hash = sha256(image)
    probe = args.probe_elf.resolve() if args.probe_elf else target_compile_probe(
        output, sysroot, args.cxx, args.ld)
    if not probe.is_file():
        raise SystemExit(f"target probe ELF is missing: {probe}")
    if args.probe_elf:
        validate_target_elf(probe, sysroot=sysroot,
                            required_needed=("libfltk.so.1.3", "libstdc++.so.6",
                                             "libgcc_s.so.1"))
    payload = payload_files(output, probe)
    disk = prepare_disk(image, kernel, output, payload)
    metadata: dict[str, object] = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "input_image_sha256": source_image_hash, "disk": disk,
        "probe_elf": str(probe), "probe_elf_sha256": sha256(probe),
        "ocr_language": "chi_sim+eng", "guest_user": "test",
        "guest_language": "zh_CN.UTF-8", "selected_apps": args.apps,
        "apps": {}, "failures": [],
    }
    (output / "run.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")

    runtime = temporary_root / f"fltk-{os.getpid():x}"
    if runtime.exists():
        raise SystemExit(f"QEMU runtime directory already exists: {runtime}")
    runtime.mkdir()
    qmp = runtime / "qmp.sock"
    if len(str(qmp)) > 108:
        raise SystemExit(f"QMP socket path exceeds Linux AF_UNIX limit: {qmp}")
    process = serial = probe_qmp = None
    failures: list[str] = []
    font_info = None
    probe_result = None
    try:
        process, serial, qmp = xorg.boot([], Path(disk["private_image"]), runtime)
        deadline = time.monotonic() + 15
        while not qmp.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before its QMP socket was ready")
            time.sleep(.1)
        probe_qmp = xorg.Probe(qmp, output)
        xorg.wait_log(serial, "boot complete:", process, timeout=args.boot_timeout)
        xorg.wait_greeter_stage(probe_qmp, process, "login", "fltk-greeter-login",
                                timeout=min(args.boot_timeout, 120))
        xorg.greeter_login(probe_qmp, process)
        xorg.wait_session(probe_qmp, process, "fltk-session-live", timeout=60)
        xorg.st_marker(probe_qmp, serial, process, "FLTK_ST_READY")
        baseline = probe_qmp.frame("desktop-baseline")

        try:
            font_info = run_font_probe(probe_qmp, serial, process)
        except (AssertionError, RuntimeError) as error:
            failures.append(f"SimSun fc-match verification failed: {error}")

        if args.skip_input_probe:
            probe_result = {"performed": False, "skipped": True,
                            "reason": "--skip-input-probe; focused app regression"}
        else:
            try:
                probe_result = probe_chinese_input(probe_qmp, serial, process,
                                                    output, failures)
                probe_exit_offset = len(serial.read_text(errors="replace"))
                probe_qmp.key("alt-f4")
                probe_pid = probe_result["pid"]
                exit_marker = f"[fltk-runner] EXIT app=fltk-ui-probe pid={probe_pid} status="
                final_probe_log = wait_marker(serial, exit_marker, process,
                                              probe_exit_offset, 20)
                exit_line = next((line for line in final_probe_log[probe_exit_offset:].splitlines()
                                  if exit_marker in line), "")
                status_match = re.search(r"\bstatus=(\d+)", exit_line)
                probe_status = int(status_match.group(1)) if status_match else None
                probe_result["exit_status"] = probe_status
                if probe_status != 0:
                    failures.append(f"FLTK input probe exited with status {probe_status!r}")
            except (AssertionError, RuntimeError) as error:
                failures.append(f"FLTK UTF-8 input/browser probe failed: {error}")
                try:
                    probe_qmp.key("alt-f4")
                except Exception:
                    pass
        for app in args.apps:
            try:
                app_result = run_one_app(
                    app, probe_qmp, serial, process, baseline, output, failures)
                metadata["apps"][app] = app_result
                if app_result.get("exit_status") is None:
                    raise RuntimeError(
                        f"stopping after {app}: app exit/window closure was not confirmed")
            except (AssertionError, RuntimeError, OSError) as error:
                failures.append(f"{app}: {error}")
                # Do not type into an unknown focused app after an interaction
                # failure or unconfirmed close; stop and preserve evidence.
                raise
        # Re-capture serial before closing QEMU; the private test commands,
        # app PIDs, ready markers, and exit statuses remain auditable.
        metadata["font_match"] = font_info
        metadata["utf8_probe"] = probe_result
    except (Exception, KeyboardInterrupt) as error:
        failures.append(f"acceptance aborted: {type(error).__name__}: {error}")
        traceback.print_exc()
    finally:
        if probe_qmp is not None:
            try:
                probe_qmp.frame("last-frame")
            finally:
                probe_qmp.close()
        if process is not None and process.poll() is None:
            try:
                qmp_quit(qmp, process)
            except Exception:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
        if serial is not None and serial.exists():
            shutil.copyfile(serial, output / "serial.log")
        qemu_log = runtime / "qemu.log"
        if qemu_log.is_file():
            shutil.copyfile(qemu_log, output / "qemu.log")
        qmp.unlink(missing_ok=True)
        try:
            runtime.rmdir()
        except OSError:
            pass

    metadata["finished_utc"] = datetime.now(timezone.utc).isoformat()
    metadata["source_image_sha256_after"] = sha256(image)
    metadata["source_image_unchanged"] = metadata["source_image_sha256_after"] == source_image_hash
    metadata["failures"] = failures
    if not metadata["source_image_unchanged"]:
        failures.append("source raw image changed during private test run")
    (output / "run.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
    if failures:
        print(f"FAIL FLTK app acceptance ({len(failures)} issues): {output}", flush=True)
        for failure in failures:
            print(f"  - {failure}", flush=True)
        return 1
    print(f"PASS FLTK app acceptance: {output}", flush=True)
    print(f"  serial: {output / 'serial.log'}", flush=True)
    print(f"  manifest: {output / 'run.json'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
