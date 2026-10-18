#!/usr/bin/env python3
"""Exercise frontend-generated commands against real apk in a temporary root."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from apk_distribution import make_package


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    args = parser.parse_args()
    apk = args.apk.resolve()
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "TMPDIR": str(temporary), "LC_ALL": "C"}

    def run(argv, **kwargs):
        result = subprocess.run(list(map(str, argv)), env=env, capture_output=True, **kwargs)
        if result.returncode:
            raise RuntimeError((argv, result.returncode, result.stdout, result.stderr))
        return result

    with tempfile.TemporaryDirectory(prefix="reliefapk-apk.", dir=temporary) as name:
        work = Path(name)
        probe = work / "command-probe"
        run([os.environ.get("HOSTCXX", "c++"), "-std=c++17", "-O2", "-fno-exceptions",
             "-fno-rtti", "-nostdinc++", "-I" + str(ROOT / "userland/apps/reliefapk"),
             ROOT / "tools/tests/reliefapk_command_probe.cpp",
             ROOT / "userland/apps/reliefapk/model.cpp", "-o", probe])
        key = work / "test-key.pem"
        run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", key])
        key.chmod(0o600)
        keys = work / "keys"
        keys.mkdir()
        run(["openssl", "pkey", "-in", key, "-pubout", "-out", keys / "test-key.rsa.pub"])
        root = work / "root"
        root.mkdir()
        download = work / "downloads"
        download.mkdir()
        repos = []
        for version in ("1.0-r0", "2.0-r0"):
            repo = work / version
            repo.mkdir()
            for package, deps in (("ra-dependency", []), ("ra-demo", ["ra-dependency"])):
                payload = work / (version + package)
                (payload / "usr/share/ra").mkdir(parents=True)
                (payload / "usr/share/ra" / package).write_text(version + "\n")
                make_package(apk, key, payload, repo / (package + "-" + version + ".apk"),
                             package, version, deps)
            run([apk, "mkndx", "--keys-dir", keys, "--sign-key", key,
                 "--output", repo / "packages.adb", *sorted(repo.glob("*.apk"))])
            repos.append(repo)
        repository = repos[0]
        run([apk, "--root", root, "--arch", "x86_64", "--initdb", "--usermode", "--keys-dir", keys,
             "--repositories-file", "/dev/null", "--no-scripts", "add"])

        checks = 0

        def command(kind, action, *targets, success=True):
            nonlocal checks
            emitted = run([probe, kind, str(action), *targets]).stdout
            generated = [v.decode() for v in emitted.rstrip(b"\0").split(b"\0")]
            actual = [apk, "--root", root, "--arch", "x86_64", "--keys-dir", keys,
                      "--repositories-file", "/dev/null", "--repository", repository / "packages.adb",
                      *generated[1:]]
            result = subprocess.run(list(map(str, actual)), env=env, capture_output=True, text=True)
            if success:
                assert result.returncode == 0, (actual, result.returncode, result.stdout, result.stderr)
            else:
                assert result.returncode != 0, (actual, result.stdout)
            checks += 1
            return result.stdout

        def query(filter):
            return json.loads(command("query", filter))

        assert {p["name"] for p in query(0)} == {"ra-demo", "ra-dependency"}
        assert query(1) == []
        metadata = json.loads(command("metadata", "ra-demo"))
        assert metadata[0]["version"] == "1.0-r0" and metadata[0]["depends"] == ["ra-dependency"]
        command("simulate", 0, "ra-demo")
        assert not (root / "usr/share/ra/ra-demo").exists()
        command("run", 0, "ra-demo")
        assert (root / "usr/share/ra/ra-demo").read_text() == "1.0-r0\n"
        assert len(query(1)) == 2
        assert {p["name"] for p in query(3)} == {"ra-demo", "ra-dependency"}
        for action in (13, 14, 15, 16, 20, 21):
            command("run", action, "ra-demo")
        command("run", 17, "/usr/share/ra/ra-demo")
        command("run", 18)
        command("run", 12)
        # apk intentionally does not support audit for a usermode database;
        # system audit is exercised against the real guest instead.
        assert command("run", 19, "1.0-r0", "2.0-r0").strip() == "<"
        command("run", 9, download, "ra-demo")
        assert len(list(download.glob("*.apk"))) == 2
        command("run", 10, repos[0] / "ra-demo-1.0-r0.apk")
        repository = repos[1]
        assert {p["name"] for p in query(2)} == {"ra-demo", "ra-dependency"}
        assert next(p for p in query(0) if p["name"] == "ra-demo")["version"] == "2.0-r0"
        command("simulate", 2, "ra-demo", "ra-dependency")
        command("run", 2, "ra-demo", "ra-dependency")
        assert (root / "usr/share/ra/ra-demo").read_text() == "2.0-r0\n"
        (root / "usr/share/ra/ra-demo").write_text("changed\n")
        command("run", 3, "ra-demo")
        assert (root / "usr/share/ra/ra-demo").read_text() == "2.0-r0\n"
        command("run", 4, "ra-demo")
        command("run", 1, "ra-demo")
        assert not (root / "usr/share/ra/ra-demo").exists()
        command("run", 0, repos[1] / "ra-demo-2.0-r0.apk")
        assert (root / "usr/share/ra/ra-demo").read_text() == "2.0-r0\n"
        command("simulate", 0, "ra-does-not-exist", success=False)
        assert (root / "usr/share/ra/ra-demo").read_text() == "2.0-r0\n"
        for filter in (0, 1, 3, 4):
            query(filter)
        print(f"ReliefAPK real apk integration: {checks} commands passed; "
              "query, install, dependencies, upgrade, reinstall, repair, removal, local APK, "
              "failure, download, verify and diagnostics verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
