#!/usr/bin/env python3
"""Website contracts without signing, a cross toolchain, Sphinx or a browser.

An optional site argument checks the real assembled fixture from test-pages.sh.
Python is only a test dependency; production renderers remain POSIX shell.
"""
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import urljoin, urlsplit

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "tools/build"
LANGUAGES = {"zh-CN": "", "en": "en/", "ja": "ja/"}
LABELS = {"zh-CN": "简体中文", "en": "English", "ja": "日本語"}
PUBLISHED_SITE = (Path(sys.argv.pop(1)).resolve()
                  if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None)


def run_script(name, *args):
    return subprocess.run(["sh", str(SCRIPTS / name), *map(str, args)],
                          text=True, capture_output=True)


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.digest()


def copy_fixture(source, destination):
    # Mutation tests only edit HTML. Share read-only artifacts so testing a real
    # published ISO/repository does not duplicate gigabytes for every case.
    if Path(source).suffix == ".html":
        return shutil.copy2(source, destination)
    os.link(source, destination)
    return destination


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__(convert_charrefs=True)
        self.language = None
        self.nav = ""
        self.anchors = []
        self.alternates = {}
        self.text = []
        self.feed(path.read_text())

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "html":
            self.language = attrs.get("lang")
        elif tag == "nav":
            self.nav = attrs.get("class", "")
        elif tag == "a":
            self.anchors.append((self.nav, attrs))
        elif tag == "link" and attrs.get("rel") == "alternate":
            language = attrs.get("hreflang")
            if language in self.alternates:
                raise AssertionError(f"duplicate alternate for {language}")
            self.alternates[language] = attrs.get("href")

    def handle_endtag(self, tag):
        if tag == "nav":
            self.nav = ""

    def handle_data(self, data):
        self.text.append(data)


def fixture(root):
    """Minimal protocol-4 bytes, sufficient for the publication checker."""
    rpr = root / "rpr"
    for directory in (rpr / "apk", rpr / "kernel", rpr / "css", root / "download", root / "css"):
        directory.mkdir(parents=True)
    files = {
        "apk/reliefos-kernel-4.9.1-r5.apk": b"signed-kernel-fixture",
        "apk/libatk-1.0-2.60.3-r0.apk": b"numbered-package-fixture",
        "apk/packages.adb": b"signed-index-fixture",
        "apk/reliefos-rpr.rsa.pub": b"public-key-fixture",
        "apk/leonos-rpr.rsa.pub": b"public-key-fixture",
        "kernel/kernel.sys": b"kernel-fixture",
        "kernel/initramfs.cpio": b"initramfs-fixture",
        "kernel/bundle.json": b'{"files":{}}\n',
        "manifest.json": b'{"apk":"/apk/packages.adb","kernel":"/kernel/release.txt","schema":1,"version":"4.9.1"}\n',
        "health.txt": b"leonos-rpr-ok\n",
        ".complete": b"4.9.1\n",
        ".nojekyll": b"",
    }
    for name, data in files.items():
        (rpr / name).write_bytes(data)
    digest = lambda name: hashlib.sha256((rpr / name).read_bytes()).hexdigest()
    package = "reliefos-kernel-4.9.1-r5.apk"
    fields = dict(format_version="4", image_version="4.9.1", version="4.9.1",
                  kernel_release="5.1.2-ko1-fixture", module_abi="0" * 64,
                  kernel_sha256=digest("kernel/kernel.sys"),
                  initramfs_sha256=digest("kernel/initramfs.cpio"), package="reliefos-kernel",
                  package_version="4.9.1-r5", package_file=package,
                  package_sha256=digest("apk/" + package), repository_sha256=digest("apk/packages.adb"),
                  bundle_manifest_sha256=digest("kernel/bundle.json"),
                  module_directory="lib/modules/5.1.2-ko1-fixture")
    (rpr / "kernel/release.txt").write_text("".join(f"{key}={value}\n" for key, value in fields.items()))
    release = dict(architecture="x86_64", schema=4, image_version="4.9.1", version="4.9.1",
                   kernel_release=fields["kernel_release"], module_abi=fields["module_abi"],
                   module_directory=fields["module_directory"],
                   files={name: {"sha256": digest("kernel/" + name)}
                          for name in ("kernel.sys", "initramfs.cpio", "bundle.json")},
                   package=dict(name="reliefos-kernel", version="4.9.1-r5", file=package,
                                sha256=fields["package_sha256"]),
                   repository_sha256=fields["repository_sha256"])
    (rpr / "kernel/release.json").write_text(json.dumps(release, separators=(",", ":")) + "\n")
    packages = sorted(path.name for path in (rpr / "apk").glob("*.apk"))
    (rpr / "apk/repository.json").write_text(json.dumps({"packages": packages}) + "\n")
    for directory, members in (("apk", packages + ["packages.adb", "reliefos-rpr.rsa.pub", "leonos-rpr.rsa.pub"]),
                               ("kernel", ["kernel.sys", "initramfs.cpio", "bundle.json"])):
        (rpr / directory / "SHA256SUMS").write_text("".join(
            f"{digest(directory + '/' + member)}  {member}\n" for member in members))
    for destination in (root / "css/reliefos.css", rpr / "css/reliefos.css"):
        shutil.copyfile(ROOT / "resources/pages/css/reliefos.css", destination)
    # Documentation is owned by a separate renderer. Only its route contract is
    # needed in this isolated fixture; test-pages.sh exercises the real manuals.
    for language, prefix in LANGUAGES.items():
        for page in (f"docs/{prefix}index.html", f"docs/reliefos/{prefix}index.html",
                     f"docs/reliefnt/{prefix}index.html"):
            path = root / page
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f'<!doctype html>\n<html lang="{language}"><head><title>Manual</title></head><body>Manual</body></html>\n')
    (root / ".nojekyll").touch()
    (root / "download/reliefos-installer.iso").write_bytes(b"installer-fixture")
    build = root.parent / "build_info.h"
    build.write_text('#define RELIEFOS_KERNEL_VERSION "4.9.1"\n')
    for script, args in (("rpr-human-pages.sh", (rpr, root)),
                         ("home-page.sh", (build, root)),
                         ("download-page.sh", (root / "download/reliefos-installer.iso", build, root))):
        result = run_script(script, *args)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


class SiteI18n(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="reliefos site i18n ")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.site = PUBLISHED_SITE or cls.work / "nested mount/ReliefOS"
        if PUBLISHED_SITE is None:
            fixture(cls.site)
        cls.logical_pages = ["index.html", "download/index.html", "rpr/index.html", "rpr/apk/index.html",
                             "rpr/packages/index.html", "rpr/kernel/index.html"]
        for apk in sorted((cls.site / "rpr/apk").glob("*.apk")):
            name = apk.name[:-4].rsplit("-", 2)[0]
            cls.logical_pages.append(f"rpr/packages/{name}.html")

    def resolve(self, current, href):
        self.assertFalse(href.startswith("/"), href)
        url = urlsplit(urljoin("https://example.test/project/ReliefOS/" + current, href))
        self.assertEqual(url.netloc, "example.test")
        self.assertTrue(url.path.startswith("/project/ReliefOS/"), href)
        return url.path.removeprefix("/project/ReliefOS/")

    def test_all_routes_same_page_switches_and_metadata(self):
        for language, prefix in LANGUAGES.items():
            for logical in self.logical_pages:
                with self.subTest(language=language, page=logical):
                    current = prefix + logical
                    page = Page(self.site / current)
                    self.assertEqual(page.language, language)
                    self.assertEqual(set(page.alternates), {*LANGUAGES, "x-default"})
                    switches = [attrs for nav, attrs in page.anchors if nav == "languages"]
                    self.assertEqual(len(switches), 3)
                    self.assertEqual(sum("aria-current" in attrs for attrs in switches), 1)
                    for alternate, alternate_prefix in LANGUAGES.items():
                        self.assertEqual(self.resolve(current, page.alternates[alternate]), alternate_prefix + logical)
                        switch, = [attrs for attrs in switches if attrs["hreflang"] == alternate]
                        self.assertEqual(switch["lang"], alternate)
                        self.assertEqual(self.resolve(current, switch["href"]), alternate_prefix + logical)
                        self.assertEqual(switch.get("aria-current"), "page" if alternate == language else None)
                    self.assertEqual(self.resolve(current, page.alternates["x-default"]), logical)
                    navigation = [attrs for nav, attrs in page.anchors if nav == "site"]
                    self.assertEqual([self.resolve(current, attrs["href"]) for attrs in navigation],
                                     [prefix + "index.html", prefix + "download/index.html", prefix + "rpr/index.html",
                                      f"docs/{prefix}index.html"])
                    for attrs in navigation:
                        self.assertEqual(attrs.get("aria-current"),
                                         "page" if self.resolve(current, attrs["href"]) == current else None)
                    for label in LABELS.values():
                        self.assertIn(label, page.text)
                    links = {self.resolve(current, attrs["href"]) for _, attrs in page.anchors
                             if not attrs["href"].startswith("https://")}
                    self.assertIn(f"docs/{prefix}index.html", links)
                    if logical == "rpr/index.html":
                        self.assertIn(f"docs/reliefos/{prefix}index.html", links)
                    if logical in {"rpr/index.html", "rpr/kernel/index.html"}:
                        self.assertIn(f"docs/reliefnt/{prefix}index.html", links)
                    if logical == "download/index.html":
                        self.assertIn("download/reliefos-installer.iso", links)
                        self.assertIn("download/SHA256SUMS", links)
                    if logical == "rpr/kernel/index.html":
                        self.assertIn("rpr/kernel/kernel.sys", links)
                        self.assertIn("rpr/kernel/initramfs.cpio", links)
                    if logical.startswith("rpr/packages/") and logical != "rpr/packages/index.html":
                        package_links = [link for link in links if link.endswith(".apk")]
                        self.assertEqual(len(package_links), 1)
                        self.assertTrue(package_links[0].startswith("rpr/apk/"))

    def test_full_visible_content_and_shared_artifacts(self):
        phrases = {
            "zh-CN": ["自由开放的操作系统。", "浏览仓库中发布的已签名 APK 软件包。", "模块与文件清单", "返回软件包列表"],
            "en": ["A free and open operating system.", "Browse the signed APK packages", "Module and file inventory", "Back to package list"],
            "ja": ["自由でオープンなオペレーティングシステム。", "署名済み APK パッケージ", "モジュールとファイルの一覧", "パッケージ一覧に戻る"],
        }
        for language, prefix in LANGUAGES.items():
            content = "\n".join(" ".join(Page(self.site / (prefix + logical)).text) for logical in self.logical_pages)
            for phrase in phrases[language]:
                self.assertIn(phrase, content)
            if prefix:
                self.assertEqual([path for path in (self.site / prefix).rglob("*")
                                  if path.is_file() and path.suffix != ".html"], [])
        resources = ROOT / "resources/pages/i18n"
        keysets = [set(re.findall(r"^\s+([a-z_]+)\)", (resources / (language + ".sh")).read_text(), re.M))
                   for language in LANGUAGES]
        self.assertEqual(keysets[0], keysets[1])
        self.assertEqual(keysets[0], keysets[2])

    def test_renderer_does_not_touch_any_machine_byte(self):
        destination = self.work / "rerendered"
        before = {str(path.relative_to(self.site)): file_digest(path)
                  for path in (self.site / "rpr").rglob("*") if path.is_file() and path.suffix != ".html"}
        result = run_script("rpr-human-pages.sh", self.site / "rpr", destination)
        self.assertEqual(result.returncode, 0, result.stderr)
        for relative, expected in before.items():
            self.assertEqual(file_digest(self.site / relative), expected, relative)
        self.assertTrue(all(path.suffix == ".html" for path in destination.rglob("*") if path.is_file()))

    def test_make_rebuilds_both_outputs_for_shared_resources(self):
        # Query real production rules, treating unrelated product artifacts as
        # already built. No recipe, signing action or release guard is executed.
        with tempfile.TemporaryDirectory(prefix="reliefos-make-i18n-") as temporary:
            fixture_root = Path(temporary)
            source = fixture_root / "os"
            for directory in (source / "Documentation", fixture_root / "nt/Documentation",
                              source / "resources/pages/i18n"):
                directory.mkdir(parents=True)
            shared = [source / path for path in (
                "tools/build/site-common.sh", "tools/build/rpr-human-pages.sh", "resources/pages/css/reliefos.css",
                "resources/pages/i18n/zh-CN.sh", "resources/pages/i18n/en.sh", "resources/pages/i18n/ja.sh",
                "resources/pages/i18n")]
            for path in shared:
                if not path.is_dir():
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
            makefile = fixture_root / "Makefile"
            makefile.write_text(f"""RELIEFOS_SRC := {source}
RELIEFNT_DIR := {fixture_root}/nt
O := {fixture_root}/out
O_META := {fixture_root}/meta
USERLAND_DIR := {fixture_root}/userland
BUILD_INFO_HEADER := {fixture_root}/build_info.h
APK_MANIFEST := {fixture_root}/apk-manifest
APK_REPOSITORY := {fixture_root}/repository
RELIEFOS_KERNEL_SYS := {fixture_root}/kernel.sys
RELIEFOS_KERNEL_PAYLOAD_MANIFEST := {fixture_root}/payload.json
RELIEFOS_KERNEL_TOOLS := {fixture_root}/kernel-tools
RELIEFOS_KERNEL_RECOVERY := {fixture_root}/kernel-recovery
INSTALLER_ISO := {fixture_root}/installer.iso
include {ROOT}/scripts/reliefos/Makefile.rpr
include {ROOT}/scripts/reliefos/Makefile.site
.PHONY: dependency-probe
dependency-probe:
""")
            database = subprocess.run(["make", "--no-print-directory", "-pRrq", "-f", str(makefile), "dependency-probe"],
                                      capture_output=True, text=True)
            self.assertEqual(database.returncode, 0, database.stderr)
            for relative in ("out/rpr-pages/.complete", "out/pages/.site-complete"):
                with self.subTest(target=relative):
                    target = fixture_root / relative
                    declaration = next(line for line in database.stdout.splitlines() if line.startswith(str(target) + ":"))
                    dependencies = set(declaration.split(":", 1)[1].split())
                    self.assertTrue(set(map(str, shared)) <= dependencies, declaration)
                    for dependency in dependencies:
                        path = Path(dependency)
                        if not path.exists():
                            path.parent.mkdir(parents=True, exist_ok=True)
                            path.touch()
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.touch()
                    # GNU Make also requires every peer of a grouped target.
                    peer = target.with_name("manifest.json" if target.name == ".complete" else "index.html")
                    peer.touch()
                    # --assume-old skips a prerequisite's recipe, but its real
                    # timestamp still participates in the parent's comparison.
                    for dependency in dependencies:
                        os.utime(dependency, (1_000_000_000, 1_000_000_000))
                    os.utime(target, (1_100_000_000, 1_100_000_000))
                    os.utime(peer, (1_100_000_000, 1_100_000_000))
                    query = ["make", "--no-print-directory", "-q", "-f", str(makefile), str(target)]
                    query += ["--assume-old=" + dependency for dependency in dependencies - set(map(str, shared))]
                    current = subprocess.run(query, capture_output=True, text=True)
                    self.assertEqual(current.returncode, 0, current.stderr)
                    for path in shared:
                        with self.subTest(changed=str(path.relative_to(source))):
                            os.utime(path, (1_200_000_000, 1_200_000_000))
                            outdated = subprocess.run(query, capture_output=True, text=True)
                            self.assertEqual(outdated.returncode, 1, outdated.stderr)
                            os.utime(path, (1_000_000_000, 1_000_000_000))

    def test_verifier_accepts_complete_tree(self):
        result = run_script("verify-pages.sh", self.site)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_verifier_rejects_contract_violations(self):
        changes = {
            "wrong-language": ("en/rpr/kernel/index.html", 'lang="en"', 'lang="ja"', "incorrect language"),
            "wrong-alternate": ("ja/download/index.html", 'hreflang="en" href="../../en/download/index.html"',
                                'hreflang="en" href="../../en/index.html"', "incorrect en alternate"),
            "wrong-switch": ("en/rpr/kernel/index.html", '<a href="../../../ja/rpr/kernel/index.html"',
                             '<a href="../../../ja/index.html"', "incorrect ja language switch"),
            "wrong-current": ("ja/index.html", 'hreflang="ja" aria-current="page"', 'hreflang="ja"', "incorrect ja language switch"),
            "wrong-docs": ("en/rpr/kernel/index.html", '../../../docs/reliefnt/en/index.html',
                           '../../../docs/reliefnt/index.html', "incorrect reliefnt manual language"),
            "fragment-link": ("index.html", '<main>', '<main><a href="missing.html#section">bad</a>', "broken href"),
            "absolute-link": ("index.html", '<main>', '<main><a href="/download/index.html">bad</a>', "root-relative href"),
        }
        for name, (page, old, new, error) in changes.items():
            with self.subTest(name=name):
                destination = self.work / name
                shutil.copytree(self.site, destination, copy_function=copy_fixture)
                target = destination / page
                content = target.read_text()
                self.assertIn(old, content)
                target.write_text(content.replace(old, new))
                result = run_script("verify-pages.sh", destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
        for name in ("missing-package", "copied-artifact"):
            with self.subTest(name=name):
                destination = self.work / name
                shutil.copytree(self.site, destination, copy_function=copy_fixture)
                if name == "missing-package":
                    logical = next(page for page in self.logical_pages if page.startswith("rpr/packages/") and not page.endswith("/index.html"))
                    for prefix in LANGUAGES.values():
                        (destination / (prefix + logical)).unlink()
                    expected = "missing " + logical
                else:
                    shutil.copyfile(destination / "download/SHA256SUMS", destination / "en/download/SHA256SUMS")
                    expected = "must share root artifacts"
                result = run_script("verify-pages.sh", destination)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
