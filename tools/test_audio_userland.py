#!/usr/bin/env python3
"""Run real-libasound client tests with an intercepted control backend."""
import argparse
import datetime
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def controls():
    qa = ROOT / "out/audio-hda/qa"
    qa.mkdir(parents=True, exist_ok=True)
    audit = ROOT / ".superpowers/sdd/2026-09-30-intel-hda-audio"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    log = audit / f"task-14-controls-{stamp}.log"
    with tempfile.TemporaryDirectory(prefix="controls-", dir=qa) as work, log.open("x") as output:
        common = ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                  "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                  "-I" + str(ROOT / "include"),
                  "-I" + str(ROOT / "out/audio-hda/upstream/alsa-lib/root/usr/include")]
        binary = str(Path(work) / "client")
        command = common + [str(ROOT / p) for p in ["tools/tests/audio_control_client_test.c",
                           "tools/tests/audio_control_fake_alsa.c", "userland/audio/audio_control.c"]]
        command += ["-lasound", "-pthread", "-o", binary]
        output.write("BUILD " + " ".join(command) + "\n"); output.flush()
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
        failed = result.returncode != 0
        if not failed:
            for case in ["range", "shape", "errors", "events", "close"]:
                output.write("CASE " + case + "\n"); output.flush()
                result = subprocess.run([binary, case], stdout=output, stderr=subprocess.STDOUT,
                                        timeout=10, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
                failed |= result.returncode != 0
                output.write(f"EXIT {result.returncode}\n"); output.flush()
            cli_object = str(Path(work) / "soundctl.o")
            command = common + ["-Dmain=soundctl_main", "-c", str(ROOT / "userland/apps/soundctl/main.c"), "-o", cli_object]
            output.write("CLI BUILD " + " ".join(command) + "\n"); output.flush()
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
            failed |= result.returncode != 0
            if result.returncode == 0:
                cli_binary = str(Path(work) / "cli-test")
                command = common + [cli_object] + [str(ROOT / p) for p in [
                    "tools/tests/soundctl_client_test.c", "tools/tests/audio_control_fake_alsa.c",
                    "userland/audio/audio_control.c", "userland/audio/audio_test.c"]]
                command += ["-lasound", "-pthread", "-lm", "-o", cli_binary]
                result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
                failed |= result.returncode != 0
                if result.returncode == 0:
                    result = subprocess.run([cli_binary], stdout=output, stderr=subprocess.STDOUT, timeout=10)
                    failed |= result.returncode != 0
                output.write(f"CLI EXIT {result.returncode}\n"); output.flush()
            tone_binary = str(Path(work) / "tone-test")
            command = common + [str(ROOT / p) for p in ["tools/tests/audio_tone_client_test.c", "userland/audio/audio_test.c"]]
            command += ["-lasound", "-lm", "-o", tone_binary]
            output.write("TONE BUILD " + " ".join(command) + "\n"); output.flush()
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
            failed |= result.returncode != 0
            if result.returncode == 0:
                result = subprocess.run([tone_binary], stdout=output, stderr=subprocess.STDOUT, timeout=10)
                failed |= result.returncode != 0
            output.write(f"TONE EXIT {result.returncode}\n"); output.flush()
        print(log.read_text())
        print(f"Evidence: {log}")
        return failed

def settings():
    '''Exercise ALSA state restore/store semantics through the OpenRC script.'''
    qa = ROOT / "out/audio-hda/qa"
    qa.mkdir(parents=True, exist_ok=True)
    audit = ROOT / ".superpowers/sdd/2026-09-30-intel-hda-audio"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    log = audit / f"task-15-settings-{stamp}.log"
    service = ROOT / "system/rootfs/etc/init.d/reliefos-audio"
    failed = False
    with tempfile.TemporaryDirectory(prefix="settings-", dir=qa) as work, log.open("x") as output:
        work = Path(work)
        bindir = work / "bin"
        bindir.mkdir()
        checkpath = bindir / "checkpath"
        checkpath.write_text("#!/bin/sh\nexit 0\n")
        checkpath.chmod(0o755)
        alsactl = bindir / "alsactl"
        alsactl.write_text(r'''#!/bin/sh
set -eu
file=
action=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -f) file=$2; shift 2 ;;
        restore|store) action=$1; shift ;;
        *) shift ;;
    esac
done
printf '%s %s\n' "$action" "$file" >> "$LOG"
if [ "$action" = store ]; then
    [ "${FAIL_STORE:-0}" != 1 ] || exit 1
    printf 'state-from-alsactl\n' > "$file"
fi
''')
        alsactl.chmod(0o755)
        runner = work / "runner.sh"
        runner.write_text(r'''#!/bin/sh
set -eu
eerror() { printf 'EERROR %s\n' "$*" >> "$LOG"; }
after() { printf 'AFTER %s\n' "$*" >> "$LOG"; }
. "$SERVICE"
state=$1
if [ "$2" = start ]; then start; else stop; fi
''')
        runner.chmod(0o755)
        env = {**os.environ, "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
               "SERVICE": str(service), "LOG": str(log)}
        def run(state, mode, extra=None):
            local_env = dict(env)
            if extra:
                local_env.update(extra)
            return subprocess.run([str(runner), str(state), mode], env=local_env,
                                  stdout=output, stderr=subprocess.STDOUT)
        missing = work / "missing.state"
        result = run(missing, "start")
        new_log = log.read_text()
        failed |= result.returncode != 0 or "restore " in new_log
        output.write("CASE missing-start PASS\n")
        present = work / "present.state"
        present.write_text("old-state\n")
        before = log.read_text()
        result = run(present, "start")
        after = log.read_text()
        failed |= result.returncode != 0 or "restore " + str(present) not in after[len(before):]
        output.write("CASE readable-start PASS\n")
        saved = work / "saved.state"
        saved.write_text("old-state\n")
        result = run(saved, "stop")
        failed |= result.returncode != 0 or saved.read_text() != "state-from-alsactl\n" or (work / "saved.state.tmp").exists()
        output.write("CASE atomic-stop PASS\n")
        saved.write_text("keep-state\n")
        result = run(saved, "stop", {"FAIL_STORE": "1"})
        failed |= result.returncode == 0 or saved.read_text() != "keep-state\n" or (work / "saved.state.tmp").exists()
        output.write("CASE failed-stop-preserves PASS\n")
        output.flush()
        print(log.read_text())
    print(f"Evidence: {log}")
    return failed

def doom_suite(kind):
    """Run the host-only deterministic Doom transport or mixer contract."""
    qa = ROOT / "out/audio-hda/qa"
    qa.mkdir(parents=True, exist_ok=True)
    audit = ROOT / ".superpowers/sdd/2026-09-30-intel-hda-audio"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    log = audit / f"task-16-doom-{kind}-{stamp}.log"
    test = ROOT / "tools/tests" / ("doom_music_test.c" if kind == "music" else f"doom_audio_{kind}_test.c")
    source = ROOT / "userland/apps/doom" / ("music.c" if kind == "music" else
        "audio_output.c" if kind == "refill" else f"audio_{kind}.c")
    with tempfile.TemporaryDirectory(prefix=f"doom-{kind}-", dir=qa) as work, log.open("x") as output:
        binary = str(Path(work) / "test")
        command = ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                   "-Werror", "-g", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", "-I" + str(ROOT / "userland/apps/doom")]
        command += [str(test)] if kind == "native" else [str(test), str(source)]
        if kind == "refill":
            # Compile the actual port update function included by the fixture.
            # Dead sections omit unrelated WAD APIs; ASan heap/stack checks stay
            # enabled without retaining the unrelated global module tables.
            command += ["-O1", "-ffunction-sections", "-fdata-sections",
                        "--param", "asan-globals=0", "-Wl,--gc-sections"]
            command += ["-I" + str(ROOT / path) for path in (
                "include", "userland/runtime/include", "kernel/reliefnt/include/uapi",
                "third_party/doomgeneric/doomgeneric",
                "out/audio-hda/upstream/nuked-opl3/root/usr/include")]
        if kind == "music":
            opl_source = next((ROOT / "out/audio-hda/upstream/nuked-opl3/work/source").glob("*/opl3.c"))
            command += [str(ROOT / "userland/apps/doom/music_opl.c"), "-I" + str(opl_source.parent),
                        str(opl_source)]
        command += ["-o", binary]
        output.write("BUILD " + " ".join(command) + "\n")
        output.flush()
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
        failed = result.returncode != 0
        if not failed:
            result = subprocess.run([binary], stdout=output, stderr=subprocess.STDOUT,
                                    timeout=30,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
            failed = result.returncode != 0
        output.write(f"EXIT {result.returncode}\n")
        output.flush()
        print(log.read_text())
    print(f"Evidence: {log}")
    return failed

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("suites", nargs="+",
                        choices=["controls", "settings", "doom-output", "doom-mixer", "doom-music", "doom-refill", "doom-native"])
    args = parser.parse_args()
    runners = {"controls": controls, "settings": settings,
               "doom-output": lambda: doom_suite("output"),
               "doom-mixer": lambda: doom_suite("mixer"),
               "doom-music": lambda: doom_suite("music"),
               "doom-refill": lambda: doom_suite("refill"),
               "doom-native": lambda: doom_suite("native")}
    raise SystemExit(any(runners[suite]() for suite in args.suites))
