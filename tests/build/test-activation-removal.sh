#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
PYTHONDONTWRITEBYTECODE=1 python3 "$src/tools/test_activation_removal.py"
