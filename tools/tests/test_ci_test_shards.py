#!/usr/bin/env python3
"""Exercise complete shard coverage and the production contract-test harness."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SELECTOR = ROOT / "tools/ci_test_shards.py"
spec = importlib.util.spec_from_file_location("ci_test_shards", SELECTOR)
shards = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shards)


def make_inventory(root):
    directory = root / "tests/build"
    directory.mkdir(parents=True)
    for name in (*sum(shards.OFFLOADED_TESTS.values(), ()), "test-core.sh"):
        (directory / name).write_text("#!/bin/sh\nset -eu\nprintf '%s\\n' \"${0##*/}\" >> \"$OBSERVED\"\n")
    return directory


class PartitionTests(unittest.TestCase):
    def test_repository_inventory_is_covered_exactly_once(self):
        groups = shards.partition(ROOT)
        inventory = {path.name for path in (ROOT / "tests/build").glob("test-*.sh")}
        combined = [name for group in shards.GROUPS[1:] for name in groups[group]]
        self.assertEqual(set(combined), inventory)
        self.assertEqual(len(combined), len(inventory))
        self.assertEqual(set(groups["all"]), inventory)

    def test_new_suites_automatically_join_core(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = make_inventory(root)
            (directory / "test-new-component.sh").touch()
            groups = shards.partition(root)
            self.assertEqual(groups["core"], ("test-core.sh", "test-new-component.sh"))

    def test_missing_or_renamed_explicit_suite_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = make_inventory(root)
            (directory / "test-pages.sh").rename(directory / "test-new-pages.sh")
            with self.assertRaisesRegex(ValueError, "Missing build suite.*test-pages.sh"):
                shards.partition(root)

    def test_duplicate_unknown_empty_and_unsafe_configuration_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            make_inventory(root)
            mappings = [dict(shards.OFFLOADED_TESTS, extra=("test-core.sh",)),
                        dict(shards.OFFLOADED_TESTS, auth=()),
                        dict(shards.OFFLOADED_TESTS, auth=("test-pages.sh",)),
                        dict(shards.OFFLOADED_TESTS, pages=("../test-pages.sh",))]
            for mapping in mappings:
                with self.subTest(mapping=mapping), self.assertRaises(ValueError):
                    shards.partition(root, mapping)

    def test_unknown_cli_group_is_rejected(self):
        result = subprocess.run([sys.executable, str(SELECTOR), "--group", "typo"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)

    def test_groups_json_matches_all_focused_groups_and_validates_inventory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = make_inventory(root)
            arguments = [sys.executable, str(SELECTOR), "--root", str(root), "--groups-json"]
            result = subprocess.run(arguments, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout), list(shards.OFFLOADED_TESTS))
            (directory / "test-pages.sh").unlink()
            failed = subprocess.run(arguments, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(failed.stdout, "")


class MakeShardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.inventory = make_inventory(self.root)
        tools = self.root / "tools"
        tools.mkdir()
        (tools / "ci_test_shards.py").write_bytes(SELECTOR.read_bytes())
        self.observed = self.root / "observed"
        self.makefile = self.root / "Makefile"
        self.makefile.write_text(
            f"RELIEFOS_SRC := {self.root}\nO := {self.root}/out\nO_HOST := $(O)/host\n"
            "RELIEFOS_LOG_SHELL = :\n"
            f"include {ROOT}/scripts/reliefos/Makefile.tests\n"
            "sdk rootfs-raw:\n\t@echo unexpected full image prerequisite >&2; exit 77\n"
            "selection:\n\t@printf '%s\\n' $(notdir $(RELIEFOS_SELECTED_BUILD_TESTS))\n")
        self.environment = {key: value for key, value in os.environ.items()
                            if key not in {"MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES",
                                           "RELIEFOS_CI_TEST_GROUP"}}
        self.environment["OBSERVED"] = str(self.observed)

    def run_make(self, *arguments, check=True):
        return subprocess.run(["make", "--no-print-directory", "-f", str(self.makefile),
                               "-j2", *arguments], cwd=self.root, env=self.environment,
                              capture_output=True, text=True, check=check)

    def test_default_selection_is_full_and_core_excludes_only_offloaded_suites(self):
        default = self.run_make("selection").stdout.splitlines()
        core = self.run_make("RELIEFOS_CI_TEST_GROUP=core", "selection").stdout.splitlines()
        self.assertEqual(set(default), {path.name for path in self.inventory.glob("test-*.sh")})
        self.assertEqual(core, ["test-core.sh"])

    def test_focused_targets_execute_their_exact_group_without_full_image_prerequisites(self):
        for group in ("kernel", "auth", "pages"):
            with self.subTest(group=group):
                self.observed.unlink(missing_ok=True)
                result = self.run_make("test-ci-" + group)
                self.assertEqual(self.observed.read_text().splitlines(), sorted(shards.OFFLOADED_TESTS[group]))
                self.assertIn("all 10 build suites are assigned exactly once", result.stdout)

    def test_invalid_group_and_missing_suite_fail_before_running_any_suite(self):
        for value in ("typo", "core extra", "", "%"):
            with self.subTest(value=value):
                result = self.run_make("RELIEFOS_CI_TEST_GROUP=" + value, "test-ci-kernel", check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.observed.exists())
        (self.inventory / "test-auth-stage.sh").unlink()
        result = self.run_make("test-ci-kernel", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing build suite", result.stderr)
        self.assertFalse(self.observed.exists())

    def test_focused_dry_query_touch_modes_do_not_execute_suites(self):
        for option in ("-n", "-q", "-t"):
            with self.subTest(option=option):
                self.run_make(option, "test-ci-pages", check=False)
                self.assertFalse(self.observed.exists())

    def test_nonzero_exit_and_counted_failure_both_fail_focused_gate(self):
        path = self.inventory / "test-pages.sh"
        for script in ("#!/bin/sh\nexit 1\n", "#!/bin/sh\nprintf 'FAIL - counted failure\\n'\n"):
            with self.subTest(script=script):
                path.write_text(script)
                result = self.run_make("test-ci-pages", check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("reported a failure", result.stdout)

    def test_focused_submake_retains_jobserver(self):
        child = self.root / "child.mk"
        child.write_text("check:\n\t@printf '%s\\n' '$(MAKEFLAGS)' > \"$$OBSERVED\"\n")
        (self.inventory / "test-auth-stage.sh").write_text(
            f"#!/bin/sh\nset -eu\nmake --no-print-directory -f '{child}' check\n")
        result = self.run_make("test-ci-auth")
        self.assertNotIn("jobserver unavailable", result.stderr)
        self.assertRegex(self.observed.read_text(), r"(?:^| )-j2(?: |$)")


if __name__ == "__main__":
    unittest.main()
