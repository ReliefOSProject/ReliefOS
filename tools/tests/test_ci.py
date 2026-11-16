#!/usr/bin/env python3
"""Regressions for skipped gates, cross-commit artifacts, and publication scope."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("reliefos_ci", Path(__file__).parents[1] / "ci.py")
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)
SHA = "a" * 40


class ScopeTests(unittest.TestCase):
    def select(self, paths, **kwargs):
        return ci.select_checks(paths, event="pull_request", ref="refs/pull/1/merge", **kwargs)

    def test_documentation_only_skips_product_build(self):
        self.assertEqual(self.select(["Documentation/index.rst", "Documentation/locale/en/index.po"]),
                         dict(build=False, docs=True, publish=False))

    def test_unknown_and_removed_source_paths_require_a_build(self):
        for path in ("userland/new.cpp", "old-source.c", "tools/ci.py", "scripts/ci/smoke.sh"):
            with self.subTest(path=path):
                self.assertTrue(self.select([path])["build"])

    def test_shared_inputs_verify_both_consumers(self):
        for path in ("kernel/reliefnt", "Makefile", "configs/build-version", ".github/workflows/ci.yml"):
            with self.subTest(path=path):
                self.assertEqual(self.select([path]), dict(build=True, docs=True, publish=False))

    def test_mixed_source_and_docs_changes_run_both(self):
        self.assertEqual(self.select(["Documentation/index.rst", "userland/app.c"]),
                         dict(build=True, docs=True, publish=False))

    def test_pr_never_publishes_even_with_main_ref(self):
        result = ci.select_checks(["main.c"], event="pull_request", ref="refs/heads/main")
        self.assertFalse(result["publish"])

    def test_main_push_runs_all_checks_and_publishes(self):
        self.assertEqual(ci.select_checks([], event="push", ref="refs/heads/main"),
                         dict(build=True, docs=True, publish=True))

    def test_manual_run_requires_explicit_deployment(self):
        self.assertFalse(ci.select_checks([], event="workflow_dispatch", ref="refs/heads/main")["publish"])
        self.assertTrue(ci.select_checks([], event="workflow_dispatch", ref="refs/heads/main", deploy=True)["publish"])
        with self.assertRaises(ValueError):
            ci.select_checks([], event="workflow_dispatch", ref="refs/heads/feature", deploy=True)

    def test_changes_cli_reads_deleted_and_renamed_paths_from_git(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def git(*args):
                return subprocess.check_output([
                    "git", "-c", "user.name=CI fixture", "-c", "user.email=ci@example.invalid", *args],
                    cwd=root, text=True, stderr=subprocess.PIPE).strip()

            git("init", "-q", "-b", "main")
            (root / "Documentation").mkdir()
            (root / "Documentation/removed.rst").write_text("old manual")
            (root / "old-source.c").write_text("source")
            git("add", ".")
            git("commit", "-q", "-m", "base")
            base = git("rev-parse", "HEAD")
            (root / "Documentation/removed.rst").unlink()
            git("add", "-A")
            git("commit", "-q", "-m", "remove manual")
            event, output = root / "event.json", root / "output"
            event.write_text(json.dumps({"pull_request": {"base": {"sha": base}}}))

            def selected():
                output.write_text("")
                subprocess.run([sys.executable, ci.__file__, "changes"], cwd=root, check=True,
                               capture_output=True, text=True,
                               env=dict(os.environ, GITHUB_EVENT_PATH=str(event),
                                        GITHUB_EVENT_NAME="pull_request", GITHUB_REF="refs/pull/1/merge",
                                        GITHUB_SHA=git("rev-parse", "HEAD"), GITHUB_OUTPUT=str(output)))
                return dict(line.split("=", 1) for line in output.read_text().splitlines())

            self.assertEqual(selected(), dict(build="false", docs="true", publish="false"))
            # Renaming source into the manuals must still select a build. The
            # newline also exercises Git's NUL-separated pathname transport.
            (root / "old-source.c").rename(root / "Documentation/renamed\nsource.rst")
            git("add", "-A", "--", "old-source.c", "Documentation")
            git("commit", "-q", "-m", "rename source")
            self.assertEqual(selected(), dict(build="true", docs="true", publish="false"))


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name) / "products"
        for name in ci.ARTIFACTS:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        (self.root / "kernel-install/install.bundle.json").write_text(json.dumps({"release": "5.3.3-test"}))
        self.pages = Path(self.work.name) / "pages"
        (self.pages / "download").mkdir(parents=True)
        (self.pages / "download/reliefos-installer.iso").write_bytes(b"images/reliefos-installer.iso")

    def test_round_trip_includes_hidden_config_and_boot_identity(self):
        expected = ci.write_manifest(self.root, SHA, self.pages)
        self.assertEqual(ci.verify_manifest(self.root, SHA), expected)
        self.assertEqual(json.loads((self.pages / "build-manifest.json").read_text()), expected)

    def test_different_commit_is_rejected(self):
        ci.write_manifest(self.root, SHA)
        with self.assertRaisesRegex(ValueError, "commit mismatch"):
            ci.verify_manifest(self.root, "b" * 40)

    def test_missing_empty_and_modified_images_are_rejected(self):
        image = self.root / "images/reliefos-live.iso"
        original = image.read_bytes()
        ci.write_manifest(self.root, SHA)
        for content in (None, b"", b"modified image"):
            with self.subTest(content=content):
                image.unlink(missing_ok=True)
                if content is not None:
                    image.write_bytes(content)
                with self.assertRaises(ValueError):
                    ci.verify_manifest(self.root, SHA)
                image.write_bytes(original)

    def test_incomplete_manifest_and_extra_paths_are_rejected(self):
        original = ci.write_manifest(self.root, SHA)
        for added in (False, True):
            manifest = copy.deepcopy(original)
            if added:
                manifest["files"]["../../private-key"] = manifest["files"][".config"]
            else:
                del manifest["files"][".config"]
            (self.root / ci.MANIFEST).write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "complete product set"):
                ci.verify_manifest(self.root, SHA)

    def test_symlinked_input_is_rejected(self):
        image = self.root / "images/reliefos.vmdk"
        outside = Path(self.work.name) / "outside"
        outside.write_bytes(image.read_bytes())
        ci.write_manifest(self.root, SHA)
        image.unlink()
        image.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "Symlinked artifact"):
            ci.verify_manifest(self.root, SHA)

    def test_pages_must_contain_the_same_installer(self):
        (self.pages / "download/reliefos-installer.iso").write_bytes(b"another installer")
        with self.assertRaisesRegex(ValueError, "Pages installer differs"):
            ci.write_manifest(self.root, SHA, self.pages)


class GateTests(unittest.TestCase):
    def needs(self, build=True, docs=True):
        result = {name: {"result": "success"} for name in ci.STAGES}
        result["changes"]["outputs"] = {"build": str(build).lower(), "docs": str(docs).lower(), "publish": "false"}
        if not build:
            for name in ("build", "smoke"):
                result[name]["result"] = "skipped"
        if not docs:
            result["documentation"]["result"] = "skipped"
        return result

    def test_successful_full_and_documentation_only_runs(self):
        for build, docs in ((True, True), (True, False), (False, True), (False, False)):
            self.assertTrue(ci.summary(self.needs(build, docs))[0])

    def test_failed_cancelled_or_skipped_required_stage_fails_gate(self):
        for stage in ci.STAGES:
            for result in ("failure", "cancelled", "skipped"):
                with self.subTest(stage=stage, result=result):
                    needs = self.needs()
                    needs[stage]["result"] = result
                    self.assertFalse(ci.summary(needs)[0])

    def test_missing_job_or_invalid_scope_fails_gate(self):
        needs = self.needs()
        del needs["build"]
        self.assertFalse(ci.summary(needs)[0])
        needs = self.needs()
        needs["changes"]["outputs"]["build"] = ""
        self.assertFalse(ci.summary(needs)[0])

    def test_publication_cannot_skip_product_or_documentation_checks(self):
        for build, docs in ((False, True), (True, False), (False, False)):
            needs = self.needs(build, docs)
            needs["changes"]["outputs"]["publish"] = "true"
            self.assertFalse(ci.summary(needs)[0])


class BuildHarnessTests(unittest.TestCase):
    def test_contract_submakes_keep_jobserver_and_dry_runs_execute_nothing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = Path(__file__).parents[2]
            script = root / "contract.sh"
            observed = root / "observed"
            child = root / "child.mk"
            child.write_text(f"check:\n\t@printf '%s\\n' '$(MAKEFLAGS)' > {observed}\n")
            script.write_text(f"#!/bin/sh\nset -eu\nmake --no-print-directory -f {child} check\n")
            makefile = root / "Makefile"
            makefile.write_text(
                f"RELIEFOS_SRC := {source}\nO := {root}/out\nO_HOST := $(O)/host\n"
                "RELIEFOS_LOG_SHELL = :\n"
                f"include {source}/scripts/reliefos/Makefile.tests\n"
                f"RELIEFOS_LONG_TESTS := {script}\n"
                "grub-host grub-efi: ;\n")

            def run(*arguments, check=True):
                return subprocess.run(["make", "--no-print-directory", "-f", str(makefile),
                                       "-j2", *arguments, "test-long"], cwd=root,
                                      env={key: value for key, value in os.environ.items()
                                           if key not in {"MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"}},
                                      capture_output=True, text=True, check=check)

            run("-n")
            self.assertFalse(observed.exists(), "make -n must not run contract scripts")
            run("-q", check=False)
            self.assertFalse(observed.exists(), "make -q must not run contract scripts")
            run("-t")
            self.assertFalse(observed.exists(), "make -t must not run contract scripts")
            result = run()
            self.assertNotIn("jobserver unavailable", result.stderr)
            self.assertRegex(observed.read_text(), r"(?:^| )-j2(?: |$)")
            self.assertIn("TIME - contract.sh:", result.stdout)
            # A suite that prints a failure but exits zero still fails the gate.
            script.write_text("#!/bin/sh\nprintf 'FAIL - counted failure\\n'\n")
            self.assertNotEqual(run(check=False).returncode, 0)


class MirrorTests(unittest.TestCase):
    def test_local_mirror_forces_updates_prunes_branches_and_keeps_tags(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, mirror, checkout = (root / name for name in ("source", "mirror", "checkout"))

            def git(*args, cwd=source):
                return subprocess.check_output(["git", "-c", "user.name=CI fixture",
                                                "-c", "user.email=ci@example.invalid", *map(str, args)],
                                               cwd=cwd, stderr=subprocess.PIPE, text=True).strip()

            git("init", "-q", "-b", "main", source, cwd=root)
            git("commit", "-q", "--allow-empty", "-m", "first")
            first = git("rev-parse", "HEAD")
            git("branch", "feature")
            git("tag", "source-only")
            git("clone", "-q", "--bare", source, mirror, cwd=root)
            git("branch", "obsolete", cwd=mirror)
            git("tag", "mirror-only", cwd=mirror)
            git("tag", "-d", "source-only", cwd=mirror)
            git("clone", "-q", source, checkout, cwd=root)
            git("remote", "add", "sourcehut-0", mirror, cwd=checkout)
            git("commit", "-q", "--allow-empty", "-m", "second")
            git("branch", "-D", "feature")
            script = Path(__file__).parents[2] / "scripts/ci/sourcehut-sync.sh"
            subprocess.run(["bash", str(script)], cwd=checkout,
                           env=dict(os.environ, SOURCEHUT_SYNC_COUNT="1"),
                           capture_output=True, text=True, check=True)
            self.assertEqual(git("for-each-ref", "--format=%(refname) %(objectname)", "refs/heads", cwd=mirror),
                             git("for-each-ref", "--format=%(refname) %(objectname)", "refs/heads"))
            self.assertEqual(git("tag", cwd=mirror), "mirror-only")
            self.assertEqual(git("rev-parse", "mirror-only", cwd=mirror), first)


if __name__ == "__main__":
    unittest.main()
