#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
suite=${1:-all}
run() {
    case $1 in
    model|document|snippets|dialogs|recovery-flow) sh "$src/tests/build/test-reliefmath-$1.sh";;
    renderer) sh "$src/tests/build/test-reliefmath-layout.sh" "$src/resources/reliefmath/STIXTwoMath-Regular.otf" "$src/system/fonts/simsun.ttc"; sh "$src/tests/build/test-reliefmath-backend.sh" --host;;
    protocol|process) sh "$src/tests/build/test-reliefmath-protocol.sh" "$1";;
    jobs|export|ui) sh "$src/tests/build/test-reliefmath-workflows.sh" "$1";;
    *) echo "unsupported ReliefMath suite: $1" >&2; exit 2;;
    esac
}
if [ "$suite" = all ]; then
    for selected in model document snippets dialogs renderer protocol process jobs export ui recovery-flow; do run "$selected"; done
else run "$suite"; fi
