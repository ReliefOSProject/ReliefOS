#!/bin/sh
# Isolated dependency-graph fixture; never renames/touches repository sources.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src/configs" "$tmp/src/userland/apps/one" "$tmp/src/userland/apps/two" "$tmp/out/config" "$tmp/out/host/bin" "$tmp/out/host/obj/tools/host/manifest" "$tmp/out/meta" "$tmp/musl/lib"
mkdir -p "$tmp/src/mk/components"
cp "$root/mk/components/graphics.mk" "$tmp/src/mk/components/graphics.mk"
cat >"$tmp/src/configs/components.toml" <<'DATA'
version = 1
[[components]]
id = "one"
symbol = "ONE"
kind = "program-app"
default = true
required = false
[[components]]
id = "two"
symbol = "TWO"
kind = "program-app"
default = true
required = false
[[components]]
id = "tool"
symbol = "TOOL"
kind = "tool"
default = false
required = false
DATA
printf 'main-one\n' >"$tmp/src/userland/apps/one/main.c"
printf 'extra-one\n' >"$tmp/src/userland/apps/one/extra.c"
printf 'main-two\n' >"$tmp/src/userland/apps/two/main.c"
touch "$tmp/out/config/.config" "$tmp/out/config/autoconf.h" "$tmp/runtime.so" "$tmp/png.h" "$tmp/musl.stamp"
for f in Scrt1.o crti.o crtn.o libc.so libmimalloc.so.3; do touch "$tmp/musl/lib/$f"; done
touch "$tmp/out/host/obj/tools/host/manifest/reliefos-components.c.o"
cc -std=c11 -I"$root" "$root/tools/host/manifest/reliefos-components.c" "$root/tools/host/common/buffer.c" "$root/tools/host/common/io.c" -o "$tmp/out/host/bin/reliefos-components"
cat >"$tmp/compiler" <<'DATA'
#!/bin/sh
set -eu
out= input=
while [ $# -gt 0 ]; do
 case $1 in --version) echo fixture-compiler; exit;; -print-resource-dir) echo /fixture; exit;; -o) out=$2; shift;; -c) input=$2; shift;; esac
 shift
done
cp "$input" "$out"
DATA
cat >"$tmp/linker" <<'DATA'
#!/bin/sh
set -eu
out= objects=
while [ $# -gt 0 ]; do
 case $1 in --version) echo fixture-linker; exit;; -o) out=$2; shift;; *.o) objects="$objects $1";; esac
 shift
done
cat $objects >"$out"
DATA
chmod +x "$tmp/compiler" "$tmp/linker"
cat >"$tmp/Makefile" <<'DATA'
.DEFAULT_GOAL := userland
RELIEFOS_SRC := $(FIXTURE)/src
O := $(FIXTURE)/out
O_CONFIG := $(O)/config
O_OBJ := $(O)/obj
O_META := $(O)/meta
O_HOST := $(O)/host
RELIEFOS_HOST_BIN := $(O_HOST)/bin
RELIEFOS_CONFIG_FILE := $(O_CONFIG)/.config
AUTOCONF_H := $(O_CONFIG)/autoconf.h
TARGET_CC := $(FIXTURE)/compiler
TARGET_CXX := $(FIXTURE)/compiler
TARGET_AR := ar
TARGET_LD := $(FIXTURE)/linker
MUSL_SYSROOT := $(FIXTURE)/musl
MUSL_STAMP := $(FIXTURE)/musl.stamp
RUNTIME_SO := $(FIXTURE)/runtime.so
PNG_CONFIG := $(FIXTURE)/png.h
Q := @
.PHONY: FORCE
FORCE:
define RELIEFOS_SIGNATURE_RULE
$(file >$(O_META)/$(1).candidate,$(RELIEFOS_SIG_$(1)))
$(O_META)/$(1).sig: FORCE
	@cmp -s $$@ $(O_META)/$(1).candidate || cp $(O_META)/$(1).candidate $$@
endef
include $(ROOT)/mk/userland.mk
DATA
run() { make -s -f "$tmp/Makefile" O="$tmp/out" FIXTURE="$tmp" ROOT="$root" "$@"; }
run app-one app-two
one="$tmp/out/userland/one.elf"
two="$tmp/out/userland/two.elf"
grep extra-one "$one" >/dev/null
before=$(stat -c %y "$one")
run app-one app-two
[ "$before" = "$(stat -c %y "$one")" ]
# Source deletion must remove its former contribution from the linked output.
rm "$tmp/src/userland/apps/one/extra.c"
run app-one
test "$(cat "$one")" = main-one
test "$(cat "$two")" = main-two
rm "$one"
run app-one
test "$(cat "$one")" = main-one
printf 'CONFIG_RELIEFOS_COMPONENT_ONE_BUILD=n\n' >"$tmp/out/config/.config"
printf 'upstream tool\n' >"$tmp/out/userland/tool.elf"
run userland-prune
test ! -e "$one"
test -e "$two"
test -e "$tmp/out/userland/tool.elf"
printf 'userland graph tests passed\n'
