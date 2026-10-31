#!/bin/sh
# Append the complete, component-private ReliefMath resource product to a
# rootfs staging plan. Call only after the ReliefMath component is selected.
set -eu
[ "$#" -eq 2 ] || {
    echo 'usage: reliefmath-resources-stage.sh SRC PLAN' >&2
    exit 2
}
src=${1%/}
plan=$2
[ -d "$src" ] && [ ! -L "$src" ] || {
    echo "ReliefMath source root is missing or not a directory: $src" >&2
    exit 1
}

members="resources/reliefmath/STIXTwoMath-Regular.otf resources/licenses/reliefmath/README.md resources/licenses/reliefmath/STIX-OFL.txt"
for relative in $members; do
    source=$src/$relative
    [ -f "$source" ] && [ ! -L "$source" ] || {
        echo "ReliefMath resource product is incomplete: $source" >&2
        exit 1
    }
done

if [ -e "$plan" ] || [ -L "$plan" ]; then
    [ -f "$plan" ] && [ ! -L "$plan" ] || {
        echo "ReliefMath stage plan is not a regular file: $plan" >&2
        exit 1
    }
fi

# All product members are checked before the plan is changed. These resources
# are root-owned read-only data; the shared SimSun system font is not copied.
printf 'f\t%s\t/usr/share/reliefmath/STIXTwoMath-Regular.otf\t0444\treliefmath\tunique\t0\n' \
    "$src/resources/reliefmath/STIXTwoMath-Regular.otf" >> "$plan"
printf 'f\t%s\t/usr/share/licenses/reliefmath/README.md\t0444\treliefmath\tunique\t0\n' \
    "$src/resources/licenses/reliefmath/README.md" >> "$plan"
printf 'f\t%s\t/usr/share/licenses/reliefmath/STIX-OFL.txt\t0444\treliefmath\tunique\t0\n' \
    "$src/resources/licenses/reliefmath/STIX-OFL.txt" >> "$plan"
