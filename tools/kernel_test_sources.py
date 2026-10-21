"""Locate real module inputs for host fixtures after kernel modularisation.

The white-box compositions live with the child tests; production Kbuild never
uses this helper or the compositions. This avoids testing stale copied bodies.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / 'kernel/reliefnt'


def kernel_sources(module):
    """Return each production translation unit in the named host composition."""
    header = KERNEL / 'tools/tests/modules' / (module + '.h')
    paths = [(header.parent / name).resolve() for name in
             re.findall(r'^#include "([^"]+)"', header.read_text(), re.M)]
    return [str(path.relative_to(ROOT)) for path in paths]


def module_text(module):
    """Read actual implementations and their private declarations for probes."""
    sources = [ROOT / name for name in kernel_sources(module)]
    headers = []
    for source in sources:
        for name in re.findall(r'^#include "([^"]+\.h)"', source.read_text(), re.M):
            path = (source.parent / name).resolve()
            if path.is_file() and path not in headers:
                headers.append(path)
    return '\n'.join(path.read_text() for path in [*headers, *sources])
