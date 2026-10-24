#!/usr/bin/env python3
"""Build an existing Uinxed checkout with Alpine GCC inside ReliefOS/QEMU.

Requires the current kernel, build/apk/root, and an Alpine runtime root with
gcc, binutils, make and curl (build/alpine-runtime/root by default).
The compiler runs only in the guest. The SDK builds the test launcher.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading

import test_apk_qemu as runner
from make_ext2_root import write_ext2_root
from make_live_root import make_live_tree

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--runtime-root", type=Path, default=ROOT / "build/alpine-runtime/root")
    parser.add_argument("--out", type=Path, default=ROOT / "build/uinxed-guest")
    parser.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args()
    source, work = args.source.resolve(), args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
    if dirty:
        raise RuntimeError("Use a clean Uinxed checkout to make the guest build reproducible")
    artifact = work / "UxImage"
    received = threading.Event()

    class Upload(BaseHTTPRequestHandler):
        def do_PUT(self):
            if self.path != "/UxImage":
                self.send_error(404)
                return
            try:
                remaining = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                remaining = 0
            if not 0 < remaining <= 512 * 1024 * 1024:
                self.send_error(400)
                return
            partial = artifact.with_suffix(".partial")
            with partial.open("wb") as output:
                while remaining:
                    chunk = self.rfile.read(min(remaining, 65536))
                    if not chunk:
                        self.send_error(400)
                        return
                    output.write(chunk)
                    remaining -= len(chunk)
            partial.replace(artifact)
            received.set()
            self.send_response(200)
            self.end_headers()

    server = HTTPServer(("127.0.0.1", 0), Upload)
    server.timeout = 30
    port = server.server_port
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        probe = work / "probe.c"
        command = "cd /tmp/Uinxed-Kernel && make -j2 UxImage"
        upload = ("cd /tmp/Uinxed-Kernel && sha256sum UxImage && curl --fail --noproxy '*' "
                  f"-T UxImage http://10.0.2.2:{port}/UxImage")
        probe.write_text(
            '#include <stdio.h>\n#include <stdlib.h>\n#include <time.h>\nint main(void) {\n'
            'setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);\n'
            'if (system("/tmp/pselect-probe")) return 1;\n'
            'struct timespec start, end;\n'
            'if (clock_gettime(CLOCK_MONOTONIC, &start)) return 1;\n'
            f'int result = system({json.dumps(command)});\n'
            'if (clock_gettime(CLOCK_MONOTONIC, &end)) return 1;\n'
            'printf("[uinxed-bench] build_seconds=%.3f\\n", '
            '(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9);\n'
            f'if (!result) result = system({json.dumps(upload)});\n'
            'printf("[apk-probe] DONE failures=%d status=%d\\n", result != 0, result);\n'
            'return result != 0;\n}\n', encoding="ascii")
        compiler = ROOT / "build/musl/sdk/bin/reliefos-musl-cc"
        for input_file, output in ((probe, work / "probe.elf"),
                (ROOT / "kernel/reliefnt/tools/tests/pselect_runtime_probe.c", work / "pselect-probe")):
            subprocess.run([str(compiler), "-static", "-O2", str(input_file), "-o", str(output)], check=True)
        with tempfile.TemporaryDirectory(prefix="stage-", dir=work) as temporary:
            stage = Path(temporary) / "root"
            make_live_tree(args.runtime_root.resolve(), stage)
            subprocess.run(["cp", "-a", "--remove-destination",
                            str(ROOT / "build/apk/root") + "/.", str(stage)], check=True)
            tests = stage / "usr/lib/reliefos/tests"
            tests.mkdir(parents=True, exist_ok=True)
            shutil.copy2(work / "probe.elf", tests / "linux-inventory.elf")
            shutil.copy2(work / "pselect-probe", stage / "tmp/pselect-probe")
            shutil.copytree(source, stage / "tmp/Uinxed-Kernel", ignore=shutil.ignore_patterns(".git"))
            write_ext2_root(stage, work / "root.ext2", minimum_mib=768)
        iso = runner.iso_tools
        iso.GRUB_TEMPLATE = iso.GRUB_TEMPLATE.replace(
            "autospawn=ioctlcloexec autospawn=python315", "autospawn=inventory").replace(
            "syscall-trace=/opt/python/", "").replace("set timeout=5", "set timeout=0")
        iso.build_iso(work / "root.ext2", work / "reliefos-apk.iso", work / "grub.cfg", work)
        runner.WORK = work
        runner.guest(args.timeout)
        if not received.is_set():
            raise RuntimeError("Guest did not upload this run's UxImage")
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        serial = (work / "guest-serial.log").read_text(errors="replace")
        if not re.search(r"\b" + digest + r"\s+UxImage\b", serial):
            raise RuntimeError("Host/guest UxImage hashes disagree")
        header = subprocess.check_output(["readelf", "-h", str(artifact)], text=True)
        if "ELF64" not in header or "X86-64" not in header:
            raise RuntimeError("UxImage is not an x86-64 ELF")
        timing = re.search(r"\[uinxed-bench\] build_seconds=([\d.]+)", serial)
        if not timing:
            raise RuntimeError("Guest did not report build time")
        (work / "result.json").write_text(json.dumps({
            "source_commit": commit, "sha256": digest, "size": artifact.stat().st_size,
            "command": "make -j2 UxImage", "platform": "ReliefOS QEMU/KVM, 2 vCPUs",
            "build_seconds": float(timing.group(1)),
            "kernel_sha256": hashlib.sha256((work / "iso/leonos/kernel.sys").read_bytes()).hexdigest(),
        }, indent=2) + "\n")
        print(f"PASS guest UxImage: {artifact} sha256={digest}")
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    main()
