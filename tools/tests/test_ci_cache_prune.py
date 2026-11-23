#!/usr/bin/env python3
"""Compiler-cache pruning preserves main warm starts and unrelated caches."""
import importlib.util
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "ci_cache_prune", Path(__file__).parents[2] / "scripts/ci/cache-prune.py")
prune = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prune)


def cache(id, *, role="build", ref=prune.MAIN, size=10, key=None, created=None, accessed=None):
    return {"id": id, "key": key or f"ccache-v2-Linux-X64-{role}-{id}-1", "ref": ref,
            "size_in_bytes": size, "created_at": created or f"2026-11-{id:02d}T00:00:00Z",
            "last_accessed_at": accessed or created or f"2026-11-{id:02d}T00:00:00Z"}


class SelectionTests(unittest.TestCase):
    def test_only_our_exact_snapshot_namespace_is_eligible(self):
        untouched = [cache(1, key="ccache-v1-Linux-X64-1-1"),
                     cache(2, key="downloads-v1-Linux-X64-lock"),
                     cache(3, key="ccache-v2-unrecognized-key"),
                     cache(4, key="ccache-v2-Linux-X64-unknown-4-1")]
        result = prune.plan(untouched + [cache(5), cache(6)], budget=0)
        self.assertEqual(result["delete"], [5])
        self.assertEqual(result["protected"], [6])
        self.assertEqual(result["before"], 20)

    def test_newest_main_snapshot_is_protected_for_every_role_and_platform(self):
        items = [cache(1), cache(2), cache(3, role="kernel"), cache(4, role="auth"),
                 cache(5, role="pages"), cache(6, key="ccache-v2-Linux-ARM64-build-6-1"),
                 cache(7, ref="refs/pull/123/merge")]
        result = prune.plan(items, budget=0)
        self.assertEqual(result["protected"], [2, 3, 4, 5, 6])
        self.assertEqual(result["delete"], [1, 7])
        self.assertEqual(result["remaining"], 50)

    def test_duplicate_limit_uses_creation_time_per_ref_and_role(self):
        items = [cache(1), cache(2), cache(3), cache(4, ref="refs/pull/12/merge"),
                 cache(5, ref="refs/pull/12/merge"), cache(6, ref="refs/pull/12/merge"),
                 cache(7, role="kernel")]
        result = prune.plan(items, budget=1000, keep=2)
        self.assertEqual(result["delete"], [1, 4])
        self.assertEqual(result["protected"], [3, 7])
        self.assertEqual(result["remaining"], 50)

    def test_budget_uses_last_access_after_removing_duplicate_snapshots(self):
        items = [cache(1), cache(2), cache(3),
                 cache(4, ref="refs/pull/12/merge", accessed="2026-11-09T00:00:00Z"),
                 cache(5, role="kernel", ref="refs/pull/13/merge",
                       accessed="2026-11-08T00:00:00Z")]
        # 1 is an old duplicate, 2 has the oldest access, then 5 before 4.
        self.assertEqual(prune.plan(items, budget=20)["delete"], [1, 2, 5])

    def test_timezone_offsets_are_compared_as_instants(self):
        items = [cache(1, created="2026-11-01T10:00:00+09:00"),
                 cache(2, created="2026-11-01T02:00:00Z")]
        self.assertEqual(prune.plan(items, budget=0)["protected"], [2])

    def test_repeated_pages_do_not_double_count_and_bad_metadata_fails_closed(self):
        item = cache(1)
        self.assertEqual(prune.plan([item, item])["before"], 10)
        bad = dict(item, size_in_bytes=-1)
        with self.assertRaises(ValueError):
            prune.plan([bad])
        with self.assertRaises(ValueError):
            prune.plan([item, dict(item, size_in_bytes=20)])
        with self.assertRaises(ValueError):
            prune.plan([cache(1, created="2026-11-01T00:00:00")])
        with self.assertRaises(ValueError):
            prune.plan([item], keep=0)
        with self.assertRaises(ValueError):
            prune.plan([item], budget=-1)


class ApiTests(unittest.TestCase):
    def test_all_metadata_pages_are_read(self):
        calls = []
        first = [cache(1)] * 100
        second = [cache(2)]

        def api(endpoint):
            calls.append(endpoint)
            return {"actions_caches": first if endpoint.endswith("page=1") else second}

        self.assertEqual(prune.list_caches("owner/repo", api), first + second)
        self.assertEqual(calls, [
            "repos/owner/repo/actions/caches?key=ccache-v2-&per_page=100&page=1",
            "repos/owner/repo/actions/caches?key=ccache-v2-&per_page=100&page=2"])

    def test_default_cli_is_dry_run_and_main_only_apply_is_explicit(self):
        with patch.object(prune, "list_caches", return_value=[cache(1), cache(2)]), \
                patch.object(prune, "delete_cache") as delete, \
                patch.dict(os.environ, {}, clear=True):
            self.assertEqual(prune.main(["--repo", "owner/repo", "--keep", "1"]), 0)
            delete.assert_not_called()
            with self.assertRaises(ValueError):
                prune.main(["--repo", "owner/repo", "--apply"])

    def test_apply_deletes_only_selected_ids_and_never_the_protected_snapshot(self):
        environment = {"GITHUB_REF": prune.MAIN, "GITHUB_EVENT_NAME": "push",
                       "GITHUB_REPOSITORY": "owner/repo"}
        with patch.object(prune, "list_caches", return_value=[cache(1), cache(2)]), \
                patch.object(prune, "delete_cache") as delete, \
                patch.dict(os.environ, environment, clear=True):
            self.assertEqual(prune.main(["--keep", "1", "--apply"]), 0)
            delete.assert_called_once_with("owner/repo", 1)
            with self.assertRaises(ValueError):
                prune.main(["--repo", "other/repo", "--apply"])
            os.environ["GITHUB_EVENT_NAME"] = "pull_request"
            with self.assertRaises(ValueError):
                prune.main(["--apply"])


class DeletionTests(unittest.TestCase):
    def result(self, status, *, stderr=""):
        return subprocess.CompletedProcess(
            ["gh", "api"], 0 if status == 204 else 1,
            stdout=f"HTTP/2.0 {status} Response\nContent-Length: 0\n\n", stderr=stderr)

    def test_success_and_already_evicted_cache_both_succeed(self):
        for status in (204, 404):
            with self.subTest(status=status), \
                    patch.object(prune.subprocess, "run", return_value=self.result(status)) as request:
                prune.delete_cache("owner/repo", 1)
                request.assert_called_once_with([
                    "gh", "api", "repos/owner/repo/actions/caches/1", "--method", "DELETE",
                    "--include", "--silent"], capture_output=True, text=True)

    def test_auth_rate_limit_and_server_failures_are_preserved(self):
        for status in (401, 403, 429, 500, 503):
            with self.subTest(status=status), \
                    patch.object(prune.subprocess, "run", return_value=self.result(status)), \
                    self.assertRaises(subprocess.CalledProcessError) as error:
                prune.delete_cache("owner/repo", 1)
            self.assertIn(f"HTTP/2.0 {status}", error.exception.stdout)

    def test_failure_classification_uses_response_headers_not_stderr(self):
        response = self.result(403, stderr="gh: requested cache 404 was forbidden\n")
        with patch.object(prune.subprocess, "run", return_value=response), \
                patch.object(prune.sys.stderr, "write"), \
                self.assertRaises(subprocess.CalledProcessError):
            prune.delete_cache("owner/repo", 1)
        for stdout in ("", "404 Not Found\n", '{"message":"HTTP/2.0 404 Not Found"}'):
            response = subprocess.CompletedProcess(["gh", "api"], 1, stdout=stdout, stderr="")
            with self.subTest(stdout=stdout), \
                    patch.object(prune.subprocess, "run", return_value=response), \
                    self.assertRaises(subprocess.CalledProcessError):
                prune.delete_cache("owner/repo", 1)


if __name__ == "__main__":
    unittest.main()
