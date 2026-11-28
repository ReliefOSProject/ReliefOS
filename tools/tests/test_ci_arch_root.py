#!/usr/bin/env python3
"""Check the generic host entry without contacting a Docker daemon."""
import json
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipIf(os.getuid() == 0, "the Arch build entry requires an unprivileged caller")
class ArchRootTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="reliefos-test-arch-root-")
        self.addCleanup(directory.cleanup)
        self.work = Path(directory.name)
        self.source = self.work / "source"
        # The workflow orchestration job does not clone the real submodules.
        # Use the actual Make entry and script with a small initialized checkout.
        for name in ("Makefile", "Kconfig", "Kconfig.components", "scripts/reliefos/Makefile.host",
                     "tools/build/arch-root.Dockerfile", "tools/build/arch-root.sh"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
        kernel = self.source / "kernel/reliefnt"
        kernel.mkdir(parents=True)
        (kernel / "Makefile").touch()
        for checkout in (self.source, kernel):
            subprocess.run(["git", "init", "--quiet", "--template=", str(checkout)], check=True)
        self.output = self.work / "arch-out"
        self.calls = self.work / "docker.jsonl"
        docker = self.work / "docker"
        docker.write_text(f"#!{sys.executable}\n" + """\
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
log = Path(os.environ['TEST_DOCKER_LOG'])
with log.open('a') as stream:
    stream.write(json.dumps(args) + '\\n')
if args[0] == 'context':
    print(os.environ.get('TEST_DOCKER_ENDPOINT', 'unix:///var/run/docker.sock'))
elif args[0] == 'info':
    print('29.0.0')
    print(os.environ.get('TEST_DOCKER_SECURITY', 'name=seccomp,profile=builtin'))
elif args[:2] == ['image', 'inspect']:
    sys.exit(0 if log.with_suffix('.image').exists() else 1)
elif args[0] == 'build':
    if 'pacman -Syu' not in sys.stdin.read():
        sys.exit(42)
    log.with_suffix('.image').touch()
""")
        docker.chmod(0o755)
        self.env = {**os.environ, "PATH": f"{self.work}:{os.environ['PATH']}",
                    "TEST_DOCKER_LOG": str(self.calls)}
        for name in ("DOCKER_HOST", "DOCKER_CONTEXT", "MAKEFLAGS", "MAKEOVERRIDES", "MFLAGS"):
            self.env.pop(name, None)

    def make(self, *args, **environment):
        if self.calls.exists():
            self.calls.unlink()
        result = subprocess.run(["make", "--no-print-directory", f"O={self.output}", *args],
                                cwd=self.source, env={**self.env, **environment}, text=True, capture_output=True)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []
        return result, calls

    def test_doctor_and_dry_run_do_not_create_or_download(self):
        result, calls = self.make("arch-doctor")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual({call[0] for call in calls}, {"context", "info"})
        result, calls = self.make("-n", "arch-fetch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [])
        self.assertFalse(self.output.exists())

    def test_fetch_then_build_uses_a_read_only_source_and_offline_nonroot_build(self):
        result, calls = self.make("arch-build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("arch-fetch", result.stderr)
        self.assertNotIn("run", [call[0] for call in calls])
        self.assertFalse(self.output.exists())
        result, calls = self.make("arch-fetch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("build", [call[0] for call in calls])
        fetch = next(call for call in calls if call[0] == "run")
        self.assertIn("reliefnt-fetch", fetch)
        self.assertNotIn("none", fetch)
        result, calls = self.make("arch-build", "ARCH_JOBS=2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("build", [call[0] for call in calls])
        run = next(call for call in calls if call[0] == "run")
        self.assertEqual(run[run.index("--user") + 1], f"{os.getuid()}:{os.getgid()}")
        self.assertEqual(run[run.index("--network") + 1], "none")
        self.assertIn(f"type=bind,src={self.source},dst={self.source},readonly,bind-recursive=disabled", run)
        self.assertIn(f"type=bind,src={self.output},dst=/reliefos-build,bind-recursive=disabled", run)
        self.assertIn("--pull=never", run)
        self.assertIn("kernel", run)
        self.assertIn("RELIEFNT_CACHE=/reliefos-build/downloads/reliefnt", run)
        self.assertIn("-j2", run)

    def test_output_symlink_is_rejected_without_writing_the_target(self):
        target = self.work / "other"
        target.mkdir()
        self.output.symlink_to(target, target_is_directory=True)
        result, calls = self.make("arch-fetch")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink", result.stderr)
        self.assertNotIn("build", [call[0] for call in calls])
        self.assertEqual(list(target.iterdir()), [])

    def test_unsafe_engine_or_native_output_is_rejected(self):
        for environment, expected in (({"TEST_DOCKER_ENDPOINT": "tcp://example.invalid:2376"}, "local"),
                                      ({"TEST_DOCKER_SECURITY": "name=rootless"}, "UID")):
            with self.subTest(environment=environment):
                result, calls = self.make("arch-doctor", **environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertNotIn("run", [call[0] for call in calls])
        self.output.mkdir()
        marker = self.output / ".reliefos-out"
        marker.write_text("native output\n")
        result, calls = self.make("arch-fetch")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output", result.stderr)
        self.assertNotIn("build", [call[0] for call in calls])
        self.assertEqual(marker.read_text(), "native output\n")


if __name__ == "__main__":
    unittest.main()
