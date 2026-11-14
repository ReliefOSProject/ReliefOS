#!/bin/sh
# Run a documentation tool with the same Python selection in every entry point.
set -efu
if [ -n "${DOCS_PYTHON:-}" ]; then
    exec "$DOCS_PYTHON" "$@"
fi
sphinx=$(command -v "${SPHINXBUILD:-sphinx-build}") || {
    echo 'Sphinx is required for documentation; see Documentation/requirements.txt' >&2
    exit 1
}
sphinx_bin=$(CDPATH= cd -- "$(dirname "$sphinx")" && pwd -P)
for docs_python in "$sphinx_bin/python" "$sphinx_bin/python3"; do
    if [ -x "$docs_python" ]; then
        exec "$docs_python" "$@"
    fi
done
exec python3 "$@"
