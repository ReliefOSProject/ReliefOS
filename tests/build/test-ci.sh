#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
exec python3 -m unittest discover -s "$src/tools/tests" -p test_ci.py -v
