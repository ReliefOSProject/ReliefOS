#!/usr/bin/env python3
"""Build and check both published manuals without building target products."""
import argparse
from html.parser import HTMLParser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
MANUALS = {
    "reliefos": ROOT / "Documentation",
    "reliefnt": ROOT / "kernel/reliefnt/Documentation",
}


class Page(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.anchors = set()
        self.links = []
        self.errors = []
        self.feed(source)
        self.close()

    def handle_starttag(self, tag, attrs):
        if tag == "script":
            self.errors.append("script element")
        for name, value in attrs:
            if name.startswith("on"):
                self.errors.append(f"event handler {name}")
            if value is None:
                continue
            if name == "id" or (tag == "a" and name == "name"):
                self.anchors.add(value)
            if name in {"href", "src"}:
                self.links.append(value)

    handle_startendtag = handle_starttag


def check_site(directory):
    site = directory.resolve()
    errors = []
    for manual, source in MANUALS.items():
        if not (source / "index.rst").is_file():
            errors.append(f"missing documentation source: {source}")
        for document in source.rglob("*.rst"):
            output = site / manual / document.relative_to(source).with_suffix(".html")
            if not output.is_file():
                errors.append(f"missing rendered document: {output.relative_to(site)}")
    if not (site / "index.html").is_file():
        errors.append("missing documentation portal index.html")

    pages = {}
    for path in sorted(site.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix in {".js", ".mjs", ".cjs", ".md", ".rst", ".pickle"} or path.name.endswith(".rst.txt"):
            errors.append(f"source, doctree or JavaScript leaked: {path.relative_to(site)}")
        if path.suffix == ".html":
            pages[path] = Page(path.read_text(encoding="utf-8"))

    links = 0
    for path, page in pages.items():
        name = path.relative_to(site)
        errors.extend(f"{name}: {error}" for error in page.errors)
        for href in page.links:
            links += 1
            url = urlsplit(href)
            if url.scheme.lower() == "javascript":
                errors.append(f"{name}: JavaScript URL {href}")
                continue
            if url.scheme or url.netloc:
                continue
            if url.path.startswith("/"):
                errors.append(f"{name}: link depends on the website root: {href}")
                continue
            target = (path.parent / unquote(url.path)).resolve() if url.path else path
            if not target.is_relative_to(site):
                errors.append(f"{name}: link escapes the documentation portal: {href}")
                continue
            if target.is_dir():
                target /= "index.html"
            if not target.is_file():
                errors.append(f"{name}: missing local target: {href}")
            elif url.fragment and target in pages and unquote(url.fragment) not in pages[target].anchors:
                errors.append(f"{name}: missing HTML anchor: {href}")

    if errors:
        raise RuntimeError("\n".join(errors))
    print(f"documentation: {len(pages)} HTML pages, {links} links/assets checked; "
          "both manuals complete, no JavaScript or source leaks")


def build_and_check():
    sphinx = shutil.which(os.environ.get("SPHINXBUILD", "sphinx-build"))
    if sphinx is None:
        raise RuntimeError("Sphinx is required; install Documentation/requirements.txt")
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefos-docs-", dir=temporary) as work:
        work = Path(work)
        env = dict(os.environ, TMPDIR=str(work), SPHINXBUILD=sphinx)
        for manual, source in MANUALS.items():
            subprocess.run(["make", "-C", str(source.parent), f"O={work / manual}",
                            f"SPHINXBUILD={sphinx}", "htmldocs"], check=True, env=env)
        subprocess.run(["sh", str(ROOT / "tools/build/docs-page.sh"),
                        str(MANUALS["reliefos"]), str(MANUALS["reliefnt"]),
                        str(work / "portal")], check=True, env=env)
        check_site(work / "portal")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, help="check an existing documentation portal")
    args = parser.parse_args()
    try:
        if args.site is None:
            build_and_check()
        else:
            check_site(args.site)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"documentation: {error}\n")


if __name__ == "__main__":
    main()
