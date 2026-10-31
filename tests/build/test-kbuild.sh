#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec python3 "$src/tools/test_kbuild.py"
