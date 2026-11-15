#!/usr/bin/env python3
"""Select CI checks, bind artifacts to a commit, and enforce the final gate."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


ARTIFACTS = (
    "images/reliefos.vmdk",
    "images/reliefos-live.iso",
    "images/reliefos-installer.iso",
    ".config",
    "kernel-install/install.bundle.json",
)
MANIFEST = "ci/manifest.json"
DOC_INPUTS = (
    "Documentation/", "kernel/reliefnt", "Makefile", "configs/build-version",
    "scripts/build-docs.sh", "scripts/docs-python.sh", "scripts/update-doc-translations.py",
    "scripts/reliefos/Makefile.site", "tools/build/docs-page.sh", "tools/test_documentation.py",
    ".github/workflows/check-docs.yml", ".github/workflows/ci.yml", ".github/actions/",
)
STAGES = ("changes", "checks", "documentation", "build", "smoke")


def matches(path, patterns):
    return any(path.startswith(pattern) if pattern.endswith("/") else path == pattern
               for pattern in patterns)


def select_checks(paths, *, event, ref, deploy=False):
    """Only PRs may skip product builds; main and manual runs verify everything."""
    if deploy and (event != "workflow_dispatch" or ref != "refs/heads/main"):
        raise ValueError("Manual Pages deployment requires the main branch")
    publish = ref == "refs/heads/main" and (event == "push" or deploy)
    if event != "pull_request":
        return dict(build=True, docs=True, publish=publish)
    build = docs = False
    for path in paths:
        docs = docs or matches(path, DOC_INPUTS)
        if (path.startswith("Documentation/") or path.startswith("README") or
                path in {"LICENSE", "NOTICE", ".github/README.md", ".github/PULL_REQUEST_TEMPLATE.md"} or
                path.startswith(".github/ISSUE_TEMPLATE/") or
                path in {".github/workflows/check-docs.yml", ".github/workflows/code-count.yml",
                         ".github/workflows/sourcehut-sync.yml"}):
            continue
        build = True
    return dict(build=build, docs=docs, publish=False)


def commit_sha(value):
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("Expected a full Git commit SHA")
    return value


def current_commit():
    return commit_sha(os.environ.get("GITHUB_SHA") or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True).strip())


def changes():
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    kind, ref = os.environ["GITHUB_EVENT_NAME"], os.environ["GITHUB_REF"]
    paths = []
    if kind == "pull_request":
        base = commit_sha(event["pull_request"]["base"]["sha"])
        changed = subprocess.check_output([
            "git", "diff", "--name-only", "--no-renames", "-z", base, current_commit(), "--"])
        paths = [os.fsdecode(path) for path in changed.split(b"\0") if path]
    deploy = event.get("inputs", {}).get("deploy_pages", False) in (True, "true")
    selected = select_checks(paths, event=kind, ref=ref, deploy=deploy)
    output = "".join(f"{key}={str(value).lower()}\n" for key, value in selected.items())
    print(output, end="")
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as stream:
        stream.write(output)


def regular_file(root, relative):
    path = root
    for part in Path(relative).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f"Symlinked artifact: {relative}")
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"Missing or empty artifact: {relative}")
    return path


def identity(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024**2), b""):
            digest.update(block)
    return {"size": path.stat().st_size, "sha256": digest.hexdigest()}


def kernel_release(root):
    bundle = json.loads(regular_file(root, "kernel-install/install.bundle.json").read_text())
    release = bundle["release"]
    if not isinstance(release, str) or not re.fullmatch(r"[A-Za-z0-9._+-]{1,31}", release):
        raise ValueError("Invalid kernel release in build manifest")
    return release


def write_manifest(root, commit, pages=None):
    manifest = {"schema": 1, "commit": commit_sha(commit), "kernel_release": kernel_release(root),
                "files": {name: identity(regular_file(root, name)) for name in ARTIFACTS}}
    if pages is not None:
        published = identity(regular_file(pages, "download/reliefos-installer.iso"))
        if published != manifest["files"]["images/reliefos-installer.iso"]:
            raise ValueError("Pages installer differs from the boot-test artifact")
    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    (root / MANIFEST).parent.mkdir(parents=True, exist_ok=True)
    (root / MANIFEST).write_text(text)
    if pages is not None:
        (pages / "build-manifest.json").write_text(text)
    return manifest


def verify_manifest(root, commit):
    manifest = json.loads(regular_file(root, MANIFEST).read_text())
    if manifest["schema"] != 1 or manifest["commit"] != commit_sha(commit):
        raise ValueError("Artifact schema or source commit mismatch")
    if set(manifest["files"]) != set(ARTIFACTS):
        raise ValueError("Artifact manifest does not contain the complete product set")
    for name in ARTIFACTS:
        if identity(regular_file(root, name)) != manifest["files"][name]:
            raise ValueError(f"Artifact checksum or size mismatch: {name}")
    if kernel_release(root) != manifest["kernel_release"]:
        raise ValueError("Kernel release differs from the recorded build")
    return manifest


def summary(needs):
    """A skipped required job is a failure, even if GitHub reports no failed jobs."""
    outputs = needs.get("changes", {}).get("outputs", {})
    valid_scope = all(outputs.get(key) in {"true", "false"} for key in ("build", "docs", "publish"))
    if outputs.get("publish") == "true":
        valid_scope = valid_scope and outputs.get("build") == outputs.get("docs") == "true"
    required = {"changes", "checks"}
    if outputs.get("docs") == "true":
        required.add("documentation")
    if outputs.get("build") == "true":
        required.update(("build", "smoke"))
    passed = valid_scope and set(needs) == set(STAGES)
    lines = ["## ReliefOS CI", "", "| Stage | Required | Result |", "| --- | --- | --- |"]
    for name in STAGES:
        result = needs.get(name, {}).get("result", "missing")
        passed = passed and (result == "success" if name in required else result in {"success", "skipped"})
        lines.append(f"| {name} | {'yes' if name in required else 'no'} | {result} |")
    lines += ["", "All required checks passed." if passed else "Required checks did not pass.", ""]
    return passed, "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("changes")
    commands.add_parser("summary")
    for name in ("manifest", "verify"):
        command = commands.add_parser(name)
        command.add_argument("--root", type=Path, required=True)
        command.add_argument("--commit")
        if name == "manifest":
            command.add_argument("--pages", type=Path)
    args = parser.parse_args()
    if args.command == "changes":
        changes()
    elif args.command == "summary":
        passed, report = summary(json.loads(os.environ["NEEDS_JSON"]))
        print(report)
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as stream:
                stream.write(report)
        return 0 if passed else 1
    else:
        commit = args.commit or current_commit()
        manifest = (write_manifest(args.root, commit, args.pages) if args.command == "manifest"
                    else verify_manifest(args.root, commit))
        print(f"Verified {len(manifest['files'])} products for {manifest['commit']}")
        if args.command == "manifest" and os.environ.get("GITHUB_STEP_SUMMARY"):
            with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as stream:
                stream.write(f"## Build products\n\nCommit: `{commit}`\n\n| File | Bytes | SHA-256 |\n| --- | ---: | --- |\n")
                for name, record in manifest["files"].items():
                    stream.write(f"| {name} | {record['size']} | `{record['sha256']}` |\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"CI: {error}", file=sys.stderr)
        sys.exit(1)
