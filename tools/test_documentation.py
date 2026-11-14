#!/usr/bin/env python3
"""Build and check both published manuals without building target products."""
import argparse
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
MANUALS = {
    "reliefos": ROOT / "Documentation",
    "reliefnt": ROOT / "kernel/reliefnt/Documentation",
}
LANGUAGES = {"": ("zh-CN", "简体中文"), "en": ("en", "English"), "ja": ("ja", "日本語")}


class Page(HTMLParser):
    def __init__(self, source):
        super().__init__(convert_charrefs=True)
        self.anchors = set()
        self.links = []
        self.errors = []
        self.language = None
        self.language_links = []
        self.alternates = []
        self.in_language_navigation = False
        self.current_language_link = None
        self.feed(source)
        self.close()

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == "html":
            self.language = attributes.get("lang")
        if tag == "nav" and "language-switcher" in attributes.get("class", "").split():
            self.in_language_navigation = True
            if not attributes.get("aria-label"):
                self.errors.append("language navigation has no accessible label")
        if tag == "a" and self.in_language_navigation:
            self.current_language_link = dict(attributes, label="")
            self.language_links.append(self.current_language_link)
        if tag == "link" and "alternate" in attributes.get("rel", "").split():
            self.alternates.append(attributes)
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
    def handle_endtag(self, tag):
        if tag == "nav":
            self.in_language_navigation = False
        if tag == "a":
            self.current_language_link = None

    def handle_data(self, data):
        if self.current_language_link is not None:
            self.current_language_link["label"] += data


def check_languages(path, page, family, errors):
    relative = path.relative_to(family)
    language = relative.parts[0] if relative.parts[0] in {"en", "ja"} else ""
    document = Path(*relative.parts[1:]) if language else relative
    expected_tag = LANGUAGES[language][0]
    if page.language != expected_tag:
        errors.append(f"{path}: expected HTML lang={expected_tag}, got {page.language}")
    for links, navigation in ((page.language_links, True), (page.alternates, False)):
        if len(links) != len(LANGUAGES):
            errors.append(f"{path}: expected three {'navigation' if navigation else 'alternate'} language links")
        for prefix, (tag, label) in LANGUAGES.items():
            matches = [link for link in links if link.get("hreflang") == tag]
            if len(matches) != 1:
                errors.append(f"{path}: expected one hreflang={tag} link")
                continue
            link = matches[0]
            href = urlsplit(link.get("href", ""))
            target = (path.parent / unquote(href.path)).resolve()
            if href.scheme or href.netloc or href.query or href.fragment or target != family / prefix / document:
                errors.append(f"{path}: language {tag} must link to the same document: {link}")
            if navigation:
                if link.get("lang") != tag or link.get("label", "").strip() != label:
                    errors.append(f"{path}: language link must use lang={tag} and label {label}")
                expected_current = "page" if language == prefix else None
                if link.get("aria-current") != expected_current:
                    errors.append(f"{path}: incorrect aria-current for {tag}")


def check_tree(directory, manuals, portal=False):
    site = directory.resolve()
    errors = []
    for manual, (destination, source) in manuals.items():
        if not (source / "index.rst").is_file():
            errors.append(f"missing documentation source: {source}")
        for document in source.rglob("*.rst"):
            for language in LANGUAGES:
                output = destination / language / document.relative_to(source).with_suffix(".html")
                if not output.is_file():
                    errors.append(f"missing rendered document: {output.relative_to(site)}")
    if portal:
        for language in LANGUAGES:
            if not (site / language / "index.html").is_file():
                errors.append(f"missing documentation portal {language}/index.html")

    pages = {}
    for path in sorted(site.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix in {".js", ".mjs", ".cjs", ".md", ".rst", ".pickle", ".po", ".pot", ".mo"} or path.name.endswith(".rst.txt"):
            errors.append(f"source, doctree or JavaScript leaked: {path.relative_to(site)}")
        if path.suffix == ".html":
            pages[path] = Page(path.read_text(encoding="utf-8"))

    links = 0
    for path, page in pages.items():
        name = path.relative_to(site)
        errors.extend(f"{name}: {error}" for error in page.errors)
        if not any(urlsplit(href).path.endswith("_static/manual.css") for href in page.links):
            errors.append(f"{name}: missing static manual navigation stylesheet")
        family = next((destination for destination, source in manuals.values()
                       if path.is_relative_to(destination)), site)
        check_languages(path, page, family, errors)
        if portal and family == site:
            language = name.parts[0] if name.parts[0] in {"en", "ja"} else ""
            targets = {(path.parent / unquote(urlsplit(href).path)).resolve() for href in page.links}
            for destination, source in manuals.values():
                if destination / language / "index.html" not in targets:
                    errors.append(f"{name}: missing manual link for language {language or 'zh-CN'}: {destination}")
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
          "three languages complete, no JavaScript or source leaks")


def check_site(directory):
    site = directory.resolve()
    check_tree(site, {manual: (site / manual, source) for manual, source in MANUALS.items()}, portal=True)


def compiled_sources():
    return {path: hashlib.sha256(path.read_bytes()).hexdigest()
            for source in MANUALS.values() for path in source.rglob("*.mo")}


def check_translation_tools(work, sphinx):
    """Exercise failure modes on private fixtures, never on translators' PO files."""
    from babel.messages.pofile import write_po

    def save(path, catalog):
        with path.open("wb") as stream:
            write_po(stream, catalog)

    def expect_failure(check, diagnostic):
        try:
            check()
        except RuntimeError as error:
            if diagnostic not in str(error):
                raise RuntimeError(f"expected {diagnostic}, got: {error}") from error
        else:
            raise RuntimeError(f"translation check accepted {diagnostic}")

    for manual, source in MANUALS.items():
        fixture = work / f"{manual}-fixture"
        documentation = fixture / "Documentation"
        (documentation / "nested").mkdir(parents=True)
        (fixture / "scripts").mkdir()
        (fixture / "configs").mkdir()
        for filename in ("build-docs.sh", "docs-python.sh", "update-doc-translations.py"):
            shutil.copy2(source.parent / "scripts" / filename, fixture / "scripts" / filename)
        make_inputs = ["Makefile", "scripts/build-lock.sh"]
        make_inputs += (["Kconfig", "Kconfig.components", "scripts/reliefos/Makefile.host"]
                        if manual == "reliefos" else
                        ["configs/toolchains/llvm-x86_64.mk", "scripts/Makefile.reliefnt", "tools/tests/Makefile"])
        for filename in make_inputs:
            target = fixture / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source.parent / filename, target)
        with (fixture / "scripts/build-docs.sh").open("a") as stream:
            stream.write(f'\nprintf "%s\\n" "{manual}" > "$2/.fixture-own-builder"\n')
        shutil.copy2(source / "conf.py", documentation / "conf.py")
        shutil.copytree(source / "_templates", documentation / "_templates")
        shutil.copytree(source / "_static", documentation / "_static")
        shutil.copy2(source.parent / "configs/build-version", fixture / "configs/build-version")
        (documentation / "locale").mkdir()
        policy_path = documentation / "locale/identical.json"
        shutil.copy2(source / "locale/identical.json", policy_path)
        (documentation / "index.rst").write_text(
            "测试手册\n========\n\n.. toctree::\n\n   nested/topic\n", encoding="utf-8")
        (documentation / "nested/topic.rst").write_text(
            "翻译检查\n========\n\n中文源文。\n\n注意\n\n内容\n\n``make htmldocs``\n", encoding="utf-8")
        tool = runpy.run_path(str(fixture / "scripts/update-doc-translations.py"))
        templates = tool["extract"](documentation, fixture / "gettext", sphinx)
        for template in templates.rglob("*.pot"):
            if any(message.locations for message in tool["read_catalog"](template)):
                raise RuntimeError("gettext template contains source locations")
        tool["update_catalogs"](documentation, templates)
        check = lambda: tool["check_catalogs"](documentation, templates)
        expect_failure(check, "untranslated")
        for language in ("en", "ja"):
            for path in sorted((documentation / "locale" / language).rglob("*.po")):
                catalog = tool["read_catalog"](path, language)
                for number, message in enumerate(catalog):
                    if message.id:
                        message.string = (f"English fixture {number}" if language == "en"
                                          else f"日本語の訳文 {number}") if tool["HAN"].search(message.id) else message.id
                save(path, catalog)
        check()
        for invalid in ('[]', '{}', '{"en": [], "ja": [], "zh_CN": []}',
                        '{"en": "注意", "ja": []}', '{"en": [], "ja": [1]}',
                        '{"en": [], "ja": [""]}', '{"en": [], "ja": ["注意", "注意"]}',
                        '{"en": [], "en": [], "ja": []}', '{'):
            policy_path.write_text(invalid, encoding="utf-8")
            expect_failure(check, "invalid identical whitelist")
        policy_path.unlink()
        expect_failure(check, "invalid identical whitelist")
        policy_path.write_text(json.dumps({"en": [], "ja": []}), encoding="utf-8")
        path = documentation / "locale/en/LC_MESSAGES/index.po"
        original = path.read_bytes()
        for diagnostic in ("untranslated", "fuzzy", "missing message", "obsolete message",
                           "identical Chinese source", "fuzzy header"):
            catalog = tool["read_catalog"](path, "en")
            message = next(item for item in catalog if item.id)
            if diagnostic == "untranslated":
                message.string = ""
            elif diagnostic == "fuzzy":
                message.flags.add("fuzzy")
            elif diagnostic == "missing message":
                catalog.delete(message.id, message.context)
            elif diagnostic == "obsolete message":
                catalog.add("废弃消息", "Obsolete")
            elif diagnostic == "identical Chinese source":
                message.string = message.id
            else:
                catalog.fuzzy = True
            save(path, catalog)
            before = path.read_bytes()
            expect_failure(check, diagnostic)
            if path.read_bytes() != before:
                raise RuntimeError("--check modified a PO file")
            if diagnostic == "identical Chinese source":
                tool["check_catalogs"](documentation, templates, [message.id])
            path.write_bytes(original)
        path.unlink()
        expect_failure(check, "missing PO")
        path.write_bytes(original)
        obsolete = path.with_name("obsolete.po")
        obsolete.write_bytes(original)
        expect_failure(check, "obsolete catalog")
        obsolete.unlink()
        # Updating unchanged templates must preserve existing translations and notes.
        catalog = tool["read_catalog"](path, "en")
        message = next(item for item in catalog if item.id)
        message.user_comments = ["fixture translator note"]
        message.locations = [("/home/fixture/Documentation/index.rst", 42)]
        translation = message.string
        save(path, catalog)
        tool["update_catalogs"](documentation, templates)
        updated = tool["read_catalog"](path, "en").get(message.id)
        if updated.string != translation or updated.user_comments != message.user_comments:
            raise RuntimeError("PO update discarded a translation or translator note")
        if b"#:" in path.read_bytes():
            raise RuntimeError("PO update retained source locations")
        japanese = documentation / "locale/ja/LC_MESSAGES/nested/topic.po"
        catalog = tool["read_catalog"](japanese, "ja")
        for term in ("注意", "内容"):
            catalog.get(term).string = term
        save(japanese, catalog)
        expect_failure(check, "identical Chinese source")
        for terms in (["注意*", "内容*"], [" 注意", "内容 "], ["注意"]):
            policy_path.write_text(json.dumps({"en": [], "ja": terms}), encoding="utf-8")
            expect_failure(check, "identical Chinese source")
        policy_path.write_text(json.dumps({"en": [], "ja": ["注意", "内容"]}), encoding="utf-8")
        check()
        english = documentation / "locale/en/LC_MESSAGES/nested/topic.po"
        english_original = english.read_bytes()
        catalog = tool["read_catalog"](english, "en")
        catalog.get("注意").string = "注意"
        save(english, catalog)
        expect_failure(check, "/en/LC_MESSAGES/nested/topic.po")

        # Poison PATH's python3: defaults must use SPHINXBUILD's virtual environment.
        trap_bin = fixture / "trap-bin"
        trap_bin.mkdir()
        trap_python = trap_bin / "python3"
        trap_python.write_text("#!/bin/sh\necho 'wrong PATH python3 selected' >&2\nexit 83\n")
        trap_python.chmod(0o755)
        env = dict(os.environ, SPHINXBUILD=sphinx, PATH=f"{trap_bin}{os.pathsep}{os.environ['PATH']}")
        for key in ("DOCS_PYTHON", "MAKEFLAGS", "MAKEOVERRIDES", "MFLAGS"):
            env.pop(key, None)
        make = ["make", "-C", str(fixture), f"O={fixture / 'make-out'}", f"SPHINXBUILD={sphinx}"]
        output = fixture / "make-out/Documentation/html"
        result = subprocess.run([*make, "htmldocs"], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        if result.returncode == 0 or "/en/LC_MESSAGES/nested/topic.po" not in result.stdout:
            raise RuntimeError(f"HTML build accepted an English copy under a Japanese exception: {result.stdout}")
        english.write_bytes(english_original)
        subprocess.run([*make, "gettext", "gettext-check", "htmldocs"], env=env, check=True)
        # A command-line override must take precedence over the detected interpreter.
        override = fixture / "override-python.sh"
        override_log = fixture / "override-python.log"
        override.write_text("#!/bin/sh\n" +
                            f"printf '%s\\n' override >> {shlex.quote(str(override_log))}\n" +
                            f"exec {shlex.quote(sys.executable)} \"$@\"\n")
        override.chmod(0o755)
        subprocess.run([*make, f"DOCS_PYTHON={override}", "gettext", "gettext-check", "htmldocs"],
                       env=env, check=True)
        if override_log.read_text().splitlines() != ["override"] * 3:
            raise RuntimeError("DOCS_PYTHON override was not shared by all three Make targets")
        check_tree(output, {manual: (output, documentation)})
        product = {"reliefos": "ReliefOS", "reliefnt": "ReliefNT"}[manual]
        for language, suffix in (("", "文档"), ("en", "Documentation"), ("ja", "ドキュメント")):
            html = (output / language / "index.html").read_text(encoding="utf-8")
            if f'<p class="blurb">{product} {suffix}</p>' not in html:
                raise RuntimeError(f"{manual}: theme description is not localized for {language or 'zh-CN'}")
        for language, text in (("en", "English fixture"), ("ja", "日本語の訳文")):
            if text not in (output / language / "nested/topic.html").read_text(encoding="utf-8"):
                raise RuntimeError(f"{manual}: {language} build silently fell back to Chinese")
        if list(documentation.rglob("*.mo")):
            raise RuntimeError("fixture build wrote MO files into the source")
        published = (output / "ja/nested/topic.html").read_bytes()
        if "注意" not in published.decode("utf-8") or "内容" not in published.decode("utf-8"):
            raise RuntimeError("Japanese HTML did not load the explicitly allowed identical translations")
        policy_path.write_text(json.dumps({"en": [], "ja": []}), encoding="utf-8")
        expect_failure(check, "/ja/LC_MESSAGES/nested/topic.po")
        result = subprocess.run([*make, "htmldocs"], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        if result.returncode == 0 or "/ja/LC_MESSAGES/nested/topic.po" not in result.stdout:
            raise RuntimeError("HTML build accepted a removed Japanese whitelist exception")
        if (output / "ja/nested/topic.html").read_bytes() != published:
            raise RuntimeError("failed whitelist check replaced the published manual")
        policy_path.write_text(json.dumps({"en": [], "ja": ["注意", "内容"]}), encoding="utf-8")
        # A failed rebuild must retain the previous complete published output.
        published = (output / "en/nested/topic.html").read_bytes()
        path.unlink()
        result = subprocess.run(["sh", str(fixture / "scripts/build-docs.sh"),
                                 str(documentation), str(output)],
                                env=dict(os.environ, SPHINXBUILD=sphinx, DOCS_PYTHON=sys.executable),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode == 0 or "missing PO" not in result.stdout:
            raise RuntimeError("HTML build accepted a missing PO file")
        if (output / "en/nested/topic.html").read_bytes() != published:
            raise RuntimeError("failed translation check replaced the published manual")
        path.write_bytes(original)
    portal = work / "fixture-portal"
    sources = {manual: work / f"{manual}-fixture/Documentation" for manual in MANUALS}
    subprocess.run(["sh", str(ROOT / "tools/build/docs-page.sh"),
                    str(sources["reliefos"]), str(sources["reliefnt"]), str(portal)], check=True,
                   env=dict(os.environ, SPHINXBUILD=sphinx, DOCS_PYTHON=sys.executable))
    check_tree(portal, {manual: (portal / manual, source) for manual, source in sources.items()}, portal=True)
    for manual, source in sources.items():
        if (portal / manual / ".fixture-own-builder").read_text().strip() != manual:
            raise RuntimeError("portal did not invoke the manual's own build script")
        if list(source.rglob("*.mo")):
            raise RuntimeError("portal build wrote MO files into the source")
    print("translation fixtures: missing, untranslated, stale, fuzzy, identical source, "
          "read-only checks, PO preservation, exact per-language whitelist, interpreter selection, "
          "actual MO loading and failed-build preservation checked")


def build_and_check():
    sphinx = shutil.which(os.environ.get("SPHINXBUILD", "sphinx-build"))
    if sphinx is None:
        raise RuntimeError("Sphinx is required; install Documentation/requirements.txt")
    temporary = ROOT / "out/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reliefos-docs-", dir=temporary) as work:
        work = Path(work)
        env = dict(os.environ, TMPDIR=str(work), SPHINXBUILD=sphinx, DOCS_PYTHON=sys.executable)
        before = compiled_sources()
        check_translation_tools(work, sphinx)
        for manual, source in MANUALS.items():
            subprocess.run(["make", "-C", str(source.parent), f"O={work / manual}",
                            f"SPHINXBUILD={sphinx}", f"DOCS_PYTHON={sys.executable}",
                            "htmldocs"], check=True, env=env)
            check_tree(work / manual / "Documentation/html",
                       {manual: (work / manual / "Documentation/html", source)})
        subprocess.run(["sh", str(ROOT / "tools/build/docs-page.sh"),
                        str(MANUALS["reliefos"]), str(MANUALS["reliefnt"]),
                        str(work / "portal")], check=True, env=env)
        check_site(work / "portal")
        if compiled_sources() != before:
            raise RuntimeError("documentation builds modified MO files in the source")


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
