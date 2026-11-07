#!/bin/sh
# OS test ownership gate; standalone ReliefNT has its own independent gate.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
python3 "$src/tools/test_test_ownership.py"
exec python3 "$src/tools/test_test_delegation.py"
