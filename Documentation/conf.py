"""Sphinx configuration for the ReliefOS manuals; no target build imports."""
from pathlib import Path
import posixpath
from urllib.parse import quote

project = "ReliefOS"
copyright = "2021-2026, ReliefOS contributors"
author = "ReliefOS contributors"
release = next(line.split("=", 1)[1] for line in
               (Path(__file__).resolve().parents[1] / "configs/build-version").read_text().splitlines()
               if line.startswith("release_version="))
version = release
needs_sphinx = "5.3"
root_doc = "index"
source_suffix = {".rst": "restructuredtext"}
language = "zh_CN"
locale_dirs = ["locale"]
gettext_compact = False
extensions = []
nitpicky = True
exclude_patterns = ["_templates/**"]
html_theme = "alabaster"
html_theme_options = {"description": "ReliefOS 文档", "page_width": "1100px"}
templates_path = ["_templates"]
html_static_path = ["_static"]
html_css_files = ["manual.css"]
html_sidebars = {"**": ["about.html", "navigation.html", "relations.html"]}
html_copy_source = False
html_show_sourcelink = False
html_show_sphinx = False
html_use_index = True
smartquotes = False


def _language_context(app, pagename, templatename, context, doctree):
    languages = (("zh_CN", "zh-CN", "简体中文", ""),
                 ("en", "en", "English", "en/"),
                 ("ja", "ja", "日本語", "ja/"))
    current = next(item for item in languages if item[0] == app.config.language)
    page = pagename + app.builder.out_suffix
    directory = posixpath.dirname(current[3] + page) or "."
    context["language"] = current[1]
    context["theme_description"] = {
        "zh_CN": "ReliefOS 文档", "en": "ReliefOS Documentation", "ja": "ReliefOS ドキュメント",
    }[current[0]]
    context["manual_languages"] = [
        {"tag": tag, "name": name, "current": code == current[0],
         "href": quote(posixpath.relpath(prefix + page, directory))}
        for code, tag, name, prefix in languages
    ]
    context["manual_language_label"] = {
        "zh_CN": "语言", "en": "Language", "ja": "言語",
    }[current[0]]


def _static_html_only(app, exception):
    # Pages is deliberately usable without JavaScript, including under a URL prefix.
    # The layout omits script tags; remove Sphinx's unused generated scripts too.
    if exception is None and app.builder.format == "html":
        for path in Path(app.outdir).rglob("*.js"):
            path.unlink()


def setup(app):
    app.connect("html-page-context", _language_context)
    app.connect("build-finished", _static_html_only)
