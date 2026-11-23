#!/usr/bin/env python3
"""Bound CI compiler-cache snapshots while retaining the newest main caches."""
import argparse
from datetime import datetime
import json
import os
import re
import subprocess
import sys


PREFIX = "ccache-v2-"
KEY = re.compile(r"ccache-v2-([A-Za-z0-9]+)-([A-Za-z0-9]+)-(build|kernel|auth|pages)-[0-9]+-[0-9]+")
MAIN = "refs/heads/main"
GIB = 1024 ** 3


def timestamp(value):
    result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if result.tzinfo is None:
        raise ValueError("Cache timestamp must include a timezone")
    return result


def plan(caches, *, budget=8 * GIB, keep=2):
    """Return eligible deletions without touching downloads or protected main caches."""
    if budget < 0 or keep < 1:
        raise ValueError("Cache budget must be nonnegative and keep must be positive")
    managed = {}
    for item in caches:
        match = KEY.fullmatch(item["key"])
        if match is None:
            continue
        if (type(item["id"]) is not int or item["id"] <= 0 or
                type(item["size_in_bytes"]) is not int or item["size_in_bytes"] < 0 or
                not isinstance(item["ref"], str)):
            raise ValueError("Invalid compiler-cache metadata")
        created = timestamp(item["created_at"])
        accessed = timestamp(item.get("last_accessed_at") or item["created_at"])
        record = dict(item, platform_role=match.groups(), created=created, accessed=accessed)
        previous = managed.get(item["id"])
        if previous is not None and previous != record:
            raise ValueError("Inconsistent duplicate cache metadata")
        managed[item["id"]] = record
    groups = {}
    protected = set()
    ordered = sorted(managed.values(), key=lambda c: (c["created"], c["id"]), reverse=True)
    for item in ordered:
        identity = item["platform_role"]
        group = groups.setdefault((identity, item["ref"]), [])
        if item["ref"] == MAIN and not group:
            protected.add(item["id"])
        group.append(item)
    selected = {item["id"] for group in groups.values() for item in group[keep:]}
    before = sum(item["size_in_bytes"] for item in managed.values())
    remaining = before - sum(managed[id]["size_in_bytes"] for id in selected)
    eligible = sorted((item for item in managed.values()
                       if item["id"] not in protected | selected),
                      key=lambda c: (c["accessed"], c["created"], c["id"]))
    for item in eligible:
        if remaining <= budget:
            break
        selected.add(item["id"])
        remaining -= item["size_in_bytes"]
    return {"delete": sorted(selected), "protected": sorted(protected),
            "before": before, "remaining": remaining}


def gh_api(endpoint, *, method="GET"):
    return json.loads(subprocess.check_output(
        ["gh", "api", endpoint, "--method", method], text=True))


def list_caches(repository, api=gh_api):
    result = []
    page = 1
    while True:
        data = api(f"repos/{repository}/actions/caches?key={PREFIX}&per_page=100&page={page}")
        items = data["actions_caches"]
        result.extend(items)
        if len(items) < 100:
            return result
        page += 1


def delete_cache(repository, id):
    """A cache evicted after listing is already in the requested absent state."""
    result = subprocess.run(
        ["gh", "api", f"repos/{repository}/actions/caches/{id}",
         "--method", "DELETE", "--include", "--silent"], capture_output=True, text=True)
    if result.returncode == 0:
        return
    first_line = result.stdout.splitlines()[0] if result.stdout else ""
    status = re.fullmatch(r"HTTP/\d+(?:\.\d+)? (\d{3})(?: .*)?", first_line)
    if status is not None and status[1] == "404":
        print(f"Cache {id} is already absent.")
        return
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--budget-gib", type=int, default=8)
    parser.add_argument("--keep", type=int, default=2)
    parser.add_argument("--apply", action="store_true", help="Delete the selected snapshots (main push only)")
    args = parser.parse_args(argv)
    if not args.repo or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo):
        raise ValueError("Expected an owner/repository name")
    if args.apply and (os.environ.get("GITHUB_REF") != MAIN or
                       os.environ.get("GITHUB_EVENT_NAME") != "push" or
                       os.environ.get("GITHUB_REPOSITORY") != args.repo):
        raise ValueError("Cache deletion requires a push workflow on its own main branch")
    result = plan(list_caches(args.repo), budget=args.budget_gib * GIB, keep=args.keep)
    print(json.dumps(dict(result, apply=args.apply), sort_keys=True))
    if args.apply:
        for id in result["delete"]:
            delete_cache(args.repo, id)
    if result["remaining"] > args.budget_gib * GIB:
        print("Newest main snapshots exceed the budget; they remain protected.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, TypeError, OSError, subprocess.CalledProcessError) as error:
        print(f"Cache maintenance: {error}", file=sys.stderr)
        sys.exit(1)
