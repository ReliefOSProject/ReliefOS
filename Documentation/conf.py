"""Sphinx configuration for the ReliefOS manuals; no target build imports."""
from pathlib import Path

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
extensions = []
nitpicky = True
exclude_patterns = ["_templates/**"]
html_theme = "alabaster"
html_theme_options = {"description": "ReliefOS Documentation", "page_width": "1100px"}
templates_path = ["_templates"]
html_sidebars = {"**": ["about.html", "navigation.html", "relations.html"]}
html_copy_source = False
html_show_sourcelink = False
html_show_sphinx = False
html_use_index = True
smartquotes = False


def _static_html_only(app, exception):
    # Pages is deliberately usable without JavaScript, including under a URL prefix.
    # The layout omits script tags; remove Sphinx's unused generated scripts too.
    if exception is None and app.builder.format == "html":
        for path in Path(app.outdir).rglob("*.js"):
            path.unlink()


def setup(app):
    app.connect("build-finished", _static_html_only)
