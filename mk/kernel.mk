# Kernel products: adapter over the standalone reliefnt kernel checkout.
#
# Phase 3 of the kernel/userland separation: this repository no longer compiles
# any kernel, driver or boot-loader source. The standalone checkout named by
# RELIEFNT_DIR owns those sources and builds kernel.sys, kernel.debug,
# kerneldebug.sys and loader.elf (the device drivers are linked into kernel.sys);
# this fragment drives that build and publishes the results to the legacy
# locations the rest of the parent build (rootfs, images, rpr) consumes. The
# parent consumes only the sub-build's `install` output and its exported headers
# (mk/headers.mk).
#
# The sub-build is asked on every invocation. A stamp keyed on a git SHA would
# miss local edits in the checkout, so incrementality is the sub-make's own
# decision (its signatures and depfiles); when it has nothing to do the ask is
# cheap. Publishing goes through reliefos-emit, so an unchanged product never
# moves mtime and never rebuilds its consumers.

# The kernel checkout. Since phase 5 the default path is the kernel/reliefnt
# git submodule (github.com/ReliefOSProject/ReliefNT); when it is not initialized
# the adapter refuses with instructions instead of letting parse-time
# inventories and config generation bury the cause in follow-on errors (see
# the guard below and the recipe guards).
RELIEFNT_DIR ?= $(if $(strip $(NTCLKS_DIR)),$(NTCLKS_DIR),$(RELIEFOS_SRC)/kernel/reliefnt)
# A checkout that has not been initialized must fail once, clearly, naming
# RELIEFNT_DIR -- a non-recursive clone otherwise died in `find` noise and a
# missing-tool cascade long before the adapter's recipe guard could speak.
# Initialized is judged the way tools/build/reliefnt-release-guard.sh judges it
# (a .git marker): a checkout at some other published SHA is initialized but
# may carry no root Makefile (history extract), and release flows must reach
# the release guard to name both SHAs instead of tripping this parse error.
# Goals that must keep working before any submodule exists are exempt, so a
# fresh machine can still run `make doctor` / `make fetch` to set up.
reliefnt_init_exempt_goals := help doctor fetch reliefnt-fetch ntclks-fetch
reliefnt_init_exempt :=
ifeq ($(MAKECMDGOALS),)
reliefnt_init_exempt := 1
else ifeq ($(words $(filter $(reliefnt_init_exempt_goals),$(MAKECMDGOALS))),$(words $(MAKECMDGOALS)))
reliefnt_init_exempt := 1
endif
ifeq ($(reliefnt_init_exempt),)
ifeq ($(wildcard $(RELIEFNT_DIR)/Makefile)$(wildcard $(RELIEFNT_DIR)/.git),)
$(error reliefnt adapter: kernel checkout not found: $(RELIEFNT_DIR)/Makefile \
(RELIEFNT_DIR=$(RELIEFNT_DIR); default submodule path: kernel/reliefnt); run `git submodule update --init --recursive` and \
`make fetch`, or point RELIEFNT_DIR at an existing checkout)
endif
endif
# Sub-build output directory: the checkout writes everything under O.
RELIEFNT_O ?= $(if $(strip $(NTCLKS_O)),$(NTCLKS_O),$(O)/reliefnt)
# The sub-make changes directory to RELIEFNT_DIR. Resolve paths in the parent
# before passing them across that boundary, including a caller's RELIEFNT_O
# override; otherwise O=out writes into kernel/reliefnt/out instead of ./out.
RELIEFNT_SUBBUILD_O := $(abspath $(RELIEFNT_O))
# DESTDIR for the sub-make's `install`; the published products are copied out
# of here.
RELIEFNT_DEST := $(O)/kernel-install
RELIEFNT_SUBBUILD_DEST := $(abspath $(RELIEFNT_DEST))

# Legacy product locations (kept stable for mk/rootfs.mk, mk/images.mk,
# mk/rpr.mk and mk/boot.mk).
RELIEFOS_KERNEL_SYS := $(O_GENERATED)/system/kernel.sys
RELIEFOS_KERNEL_DEBUG := $(O_GENERATED)/system/kernel.debug
RELIEFNT_LOADER_ELF := $(O_GENERATED)/boot/loader.elf
RELIEFNT_KERNELDEBUG_SYS := $(O_GENERATED)/system/kerneldebug.sys

# "legacy path below generated/:installed file name" per product; the sub-make's
# install writes the four products flat under $(RELIEFNT_DEST).
RELIEFNT_PUBLISH_PAIRS := system/kernel.sys:kernel.sys system/kernel.debug:kernel.debug \
	system/kerneldebug.sys:kerneldebug.sys boot/loader.elf:loader.elf

RELIEFNT_PUBLISHED := $(RELIEFOS_KERNEL_SYS) $(RELIEFOS_KERNEL_DEBUG) \
	$(RELIEFNT_KERNELDEBUG_SYS) $(RELIEFNT_LOADER_ELF)

# An explicit command-line tool override is part of the caller's intent and is
# passed to the sub-make as well (the checkout accepts the same CC/CXX/AR/
# RANLIB/LD/OBJCOPY/STRIP overrides). Everything else stays the checkout's own
# toolchain choice; only ARCH, PROFILE and SOURCE_DATE_EPOCH are pinned here.
RELIEFNT_TOOL_PASSTHRU := $(strip $(foreach tool,CC CXX AR RANLIB LD OBJCOPY STRIP, \
	$(if $(filter command line,$(origin $(tool))),$(tool)=$(strip $($(tool))))))
# Forward configured suffixes and explicit empty command-line overrides.
# Otherwise the standalone kernel's editable Makefile defaults apply.
RELIEFNT_VERSION_PASSTHRU := $(strip $(foreach suffix,EXTRAVERSION LOCALVERSION, \
	$(if $(strip $($(suffix))),$(suffix)='$($(suffix))',\
	$(if $(filter command line,$(origin $(suffix))),$(suffix)=''))))

# --- the delegation and publish rule -----------------------------------------
# Grouped targets: one recipe builds and installs the whole kernel product set
# in the sub-build, then publishes every product to its legacy path. FORCE, not
# a stamp: the checkout may carry local edits no recorded identity would notice.
#
# The first recipe line must stay a single command after its guard so the shell
# `exec`s the sub-make: it then is a direct child of this make and dies with it
# on an interrupt, instead of surviving as an orphan that would hold the
# sub-build's output lock. The guard keeps `make -n` a promise of no output:
# recipe lines that contain $(MAKE) run even under -n.
#
# Keep header installation ahead of a kernel build when both use the same
# sub-build O: the checkout's build lock rejects concurrent makes on one O.
# A pure kernel-product build at an older pin can predate the current UAPI
# whitelist, though. In that case the kernel itself can still build from its
# own sources; userland/header goals continue to require the complete export.
RELIEFNT_HEADER_EXPORT_LIST_FOR_KERNEL := $(if $(strip $(HEADER_EXPORT_LIST)),$(HEADER_EXPORT_LIST),$(RELIEFOS_SRC)/configs/header-export.list)
RELIEFNT_HEADER_EXPORT_ENTRIES_FOR_KERNEL := $(shell sed -e 's/\#.*//' -e '/^[[:space:]]*$$/d' $(RELIEFNT_HEADER_EXPORT_LIST_FOR_KERNEL))
RELIEFNT_HEADER_EXPORTS_COMPLETE := $(shell for entry in $(RELIEFNT_HEADER_EXPORT_ENTRIES_FOR_KERNEL); do test -f '$(RELIEFNT_DIR)/'$$entry || exit 1; done; printf yes)
RELIEFNT_KERNEL_ONLY_GOALS := kernel all loader kerneldebug
RELIEFNT_NON_KERNEL_GOALS := $(filter-out $(RELIEFNT_KERNEL_ONLY_GOALS),$(MAKECMDGOALS))
RELIEFNT_KERNEL_HEADER_ORDER_ONLY := | $(O)/kernel-export/manifest.txt
ifeq ($(strip $(RELIEFNT_NON_KERNEL_GOALS)),)
ifneq ($(RELIEFNT_HEADER_EXPORTS_COMPLETE),yes)
RELIEFNT_KERNEL_HEADER_ORDER_ONLY :=
endif
endif

$(RELIEFNT_PUBLISHED) &: FORCE $(RELIEFOS_EMIT) $(RELIEFNT_KERNEL_HEADER_ORDER_ONLY)
	+$(Q)case "$${MAKEFLAGS%% *}" in *n*) exit 0 ;; esac; \
	if [ ! -f '$(RELIEFNT_DIR)/Makefile' ] && [ ! -e '$(RELIEFNT_DIR)/.git' ]; then \
	    printf '%s\n' \
	        'reliefnt adapter: kernel checkout not found: $(RELIEFNT_DIR)/Makefile' \
	        '' \
	        'The kernel products are built by the reliefnt kernel checkout (the' \
	        'kernel/reliefnt git submodule since phase 5). Initialize it with' \
	        '`git submodule update --init --recursive` and run' \
	        '`make fetch`, or point RELIEFNT_DIR at an existing' \
	        'checkout, e.g. RELIEFNT_DIR=/path/to/reliefnt or RELIEFNT_DIR=.' >&2; \
	    exit 1; \
	fi; \
	exec $(MAKE) -C '$(RELIEFNT_DIR)' O='$(RELIEFNT_SUBBUILD_O)' ARCH='$(ARCH)' \
	    PROFILE='$(PROFILE)' SOURCE_DATE_EPOCH='$(or $(SOURCE_DATE_EPOCH),0)' \
	    $(RELIEFNT_TOOL_PASSTHRU) $(RELIEFNT_VERSION_PASSTHRU) all install DESTDIR='$(RELIEFNT_SUBBUILD_DEST)'
	$(Q)set -eu; \
	test -f $(RELIEFNT_DEST)/manifest.txt || { \
	    echo "reliefnt adapter: $(RELIEFNT_DEST)/manifest.txt missing after install" >&2; \
	    exit 1; }; \
	for pair in $(RELIEFNT_PUBLISH_PAIRS); do \
	    rel=$${pair%%:*}; name=$${pair#*:}; \
	    test -f $(RELIEFNT_DEST)/$$name || { \
	        echo "reliefnt adapter: installed product $$name missing from $(RELIEFNT_DEST)" >&2; \
	        exit 1; }; \
	    mkdir -p $(O_GENERATED)/$${rel%%/*}; \
	    $(RELIEFOS_EMIT) --input $(RELIEFNT_DEST)/$$name --output $(O_GENERATED)/$$rel; \
	done

# --- fetch delegation -------------------------------------------------------
# `make fetch` also fetches the kernel checkout's locked dependencies. The
# checkout's fetch is idempotent -- cached bytes are only verified -- so asking
# on every `make fetch` is cheap. A missing checkout cannot be fetched into
# existence here: keep the goal successful with a warning, so a fresh machine
# can still run `make doctor` / `make fetch` before the submodule exists (see
# reliefnt_init_exempt_goals). RELIEFNT_DIR pointing back at this repository would
# recurse into `make -C . fetch`; skip instead.
.PHONY: reliefnt-fetch ntclks-fetch
ntclks-fetch: reliefnt-fetch
reliefnt-fetch:
	+$(Q)case "$${MAKEFLAGS%% *}" in *n*) exit 0 ;; esac; \
	if [ '$(realpath $(RELIEFNT_DIR))' = '$(realpath $(RELIEFOS_SRC))' ]; then \
	    echo 'reliefnt adapter: RELIEFNT_DIR is this repository; skipping the kernel fetch' >&2; \
	    exit 0; \
	fi; \
	if [ ! -f '$(RELIEFNT_DIR)/Makefile' ] && [ ! -e '$(RELIEFNT_DIR)/.git' ]; then \
	    echo 'reliefnt adapter: kernel checkout not found: $(RELIEFNT_DIR)/Makefile; skipping the kernel fetch' >&2; \
	    echo 'Initialize it with `git submodule update --init --recursive`, or point RELIEFNT_DIR at an existing checkout' >&2; \
	    exit 0; \
	fi; \
	exec $(MAKE) -C '$(RELIEFNT_DIR)' fetch

# --- the parent-owned version header -----------------------------------------
# build_info.h is not a kernel product: the parent generates it for its own
# version consumers (mk/rpr.mk's app packages, mk/site.mk, `make build-info`).
# The kernel checkout generates its own copy inside its O.
BUILD_INFO_HEADER := $(O_INCLUDE)/generated/build_info.h
RELIEFOS_SOURCE_ID := $(shell git -C $(RELIEFOS_SRC) rev-parse --short HEAD 2>/dev/null || echo unknown)
RELIEFOS_SIG_version := source=$(RELIEFOS_SOURCE_ID)|epoch=$(SOURCE_DATE_EPOCH)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,version)))

$(BUILD_INFO_HEADER): $(RELIEFOS_SRC)/configs/build-version $(RELIEFOS_VERSION_TOOL) $(O_META)/version.sig \
	| $(O_INCLUDE)/generated
	$(call RELIEFOS_LOG,GEN,$@)
	$(Q)$(RELIEFOS_VERSION_TOOL) --version-file $< \
        --source-id '$(RELIEFOS_SOURCE_ID)' \
	    --epoch '$(or $(SOURCE_DATE_EPOCH),$(shell git -C $(RELIEFOS_SRC) show -s --format=%ct HEAD 2>/dev/null || echo 0))' \
	    --output $@
