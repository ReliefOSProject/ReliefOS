# ReliefOS build entry point.
#
# GNU Make owns the dependency graph, the parallel schedule and the incremental
# decisions. C helpers under tools/host/ perform data transforms only; short
# scripts under tools/build/ drive third-party builds. Nothing here shells out to
# a second scheduler. OS compilation runs no Python, Meson or Ninja; Sphinx
# serves only the separate documentation and Pages targets.
#
# Kbuild migration: Documentation/process/testing.rst

# --- GNU Make version ------------------------------------------------------
# Grouped targets ('&:') and the $(file) function both need 4.3.
reliefos_make_min := $(shell printf '4.3\n$(MAKE_VERSION)\n' | LC_ALL=C sort -V | head -n1)
ifeq ($(reliefos_make_min),4.3)
else
$(error GNU Make >= 4.3 is required, this is $(MAKE_VERSION))
endif

RELIEFOS_SRC := $(patsubst %/,%,$(dir $(realpath $(firstword $(MAKEFILE_LIST)))))

# --- user-facing variables --------------------------------------------------
# ARCH, PROFILE and O may come from the command line or from these defaults
# only. An inherited environment value is ignored on purpose: an unrelated shell
# setting must not silently change what gets built (plan section 6.2).
ifneq ($(origin ARCH),command line)
override ARCH := x86_64
endif
ifneq ($(origin PROFILE),command line)
override PROFILE := release
endif
ifneq ($(origin O),command line)
override O := $(RELIEFOS_SRC)/out/$(ARCH)/$(PROFILE)
endif

V ?= 0
CPUS ?=
MEMORY ?=
# Kernel-only suffixes; an empty default leaves the standalone kernel's
# Makefile defaults in effect. Package release versions remain numeric.
EXTRAVERSION ?=
LOCALVERSION ?=
SOURCE_DATE_EPOCH ?= $(shell git -C $(RELIEFOS_SRC) show -s --format=%ct HEAD 2>/dev/null || echo 0)
TOOLCHAIN ?= $(RELIEFOS_SRC)/configs/toolchains/llvm-x86_64.mk

O := $(patsubst %/,%,$(O))

# --- input validation -------------------------------------------------------
# The supported character set is enumerated rather than promising arbitrary
# paths: Make word splitting, shell quoting and the generated manifests all break
# on the rejected set, so failing here is far cheaper than failing mid-build.
RELIEFOS_ALLOWED_CHARS := a b c d e f g h i j k l m n o p q r s t u v w x y z \
	A B C D E F G H I J K L M N O P Q R S T U V W X Y Z \
	0 1 2 3 4 5 6 7 8 9 . _ / -
RELIEFOS_TOOL_SAFE_CHARS := $(RELIEFOS_ALLOWED_CHARS) + = : @
reliefos_empty :=
reliefos_space := $(reliefos_empty) $(reliefos_empty)
reliefos_comma := ,

# $(call strip_allowed,text,chars): keep only characters outside the allow-list.
strip_allowed = $(if $(2),$(call strip_allowed,$(subst $(firstword $(2)),,$(1)),$(wordlist 2,9999,$(2))),$(1))

reliefos_suffix_residual := $(call strip_allowed,$(EXTRAVERSION)$(LOCALVERSION),$(filter-out /,$(RELIEFOS_ALLOWED_CHARS)) +)
ifneq ($(reliefos_suffix_residual),)
$(error EXTRAVERSION and LOCALVERSION accept only A-Z a-z 0-9 . _ + -)
endif

ifeq ($(O),)
$(error O= must not be empty; it names this build's output directory)
endif
ifneq ($(words $(O)),1)
$(error O='$(O)' is unsupported: whitespace and newlines are not accepted in an output path)
endif
reliefos_O_residual := $(call strip_allowed,$(O),$(RELIEFOS_ALLOWED_CHARS))
ifneq ($(reliefos_O_residual),)
$(error O='$(O)' is unsupported: offending characters are [$(reliefos_O_residual)]; accepted are A-Z a-z 0-9 . _ / -)
endif
# Compare absolute paths: `O=.` and `O=<src>` are the same refusal. O has already
# been restricted to a safe character set above, so quoting here cannot be escaped.
reliefos_O_absolute := $(shell realpath -m -- '$(O)' 2>/dev/null || printf '%s' '$(O)')
ifeq ($(reliefos_O_absolute),/)
$(error refusing / as the output directory)
endif
ifeq ($(reliefos_O_absolute),$(RELIEFOS_SRC))
$(error refusing the source root as the output directory (O='$(O)'))
endif
ifneq ($(filter $(ARCH),x86_64),)
else
$(error unsupported ARCH '$(ARCH)'; this build supports ARCH=x86_64)
endif
ifneq ($(filter $(PROFILE),debug release),)
else
$(error unsupported PROFILE '$(PROFILE)'; use PROFILE=debug or PROFILE=release)
endif

# Reject misspelled configuration overrides before any output is created.
reliefos_config_symbols := $(shell awk '$$1 == "config" || $$1 == "menuconfig" { print "CONFIG_" $$2 }' $(RELIEFOS_SRC)/Kconfig $(RELIEFOS_SRC)/Kconfig.components)
reliefos_config_cli := $(foreach key,$(filter CONFIG_%,$(.VARIABLES)),$(if $(filter command line,$(origin $(key))),$(key)))
reliefos_config_unknown := $(filter-out CONFIG_SHELL $(reliefos_config_symbols),$(reliefos_config_cli))
ifneq ($(strip $(reliefos_config_unknown)),)
$(error unknown configuration override: $(reliefos_config_unknown))
endif

# --- output layout ----------------------------------------------------------
O_HOST      := $(O)/host
O_GENERATED := $(O)/generated
O_INCLUDE   := $(O)/include
O_CONFIG    := $(O)/config
O_SYSROOT   := $(O)/sysroot
O_STAGE     := $(O)/stage
O_PACKAGES  := $(O)/packages
O_IMAGES    := $(O)/images
O_LOGS      := $(O)/logs
O_META      := $(O)/meta
# Third-party build directories stay separate from source-relative native
# Kbuild objects under tools/ and userland/.
O_THIRD_PARTY := $(O)/third-party
# Staged root filesystem fragment for the upstream authentication libraries
# (Linux-PAM, libxcrypt, ...). Same status as third-party: a layout extension,
# because these packages install into their own DESTDIR rather than a flat
# object tree.
O_AUTH := $(O)/auth

# Shared download cache: deliberately outside O because it is profile
# independent and `distclean` must not throw it away.
RELIEFOS_CACHE ?= $(if $(strip $(LEONOS_CACHE)),$(LEONOS_CACHE),$(RELIEFOS_SRC)/cache/downloads)

# Written when an output tree is created and re-checked before anything is
# deleted, so `clean` can never operate on a directory it does not own.
RELIEFOS_O_MARKER := $(O)/.reliefos-out

# Read-only public input aliases are also available to help/doctor callers.
include $(RELIEFOS_SRC)/scripts/reliefos/Makefile.host
RELIEFOS_LOCK ?= $(if $(strip $(LEONOS_LOCK)),$(LEONOS_LOCK),$(RELIEFOS_SRC)/configs/dependencies.lock.json)
RELIEFOS_FETCH_SCRIPT ?= $(if $(strip $(LEONOS_FETCH_SCRIPT)),$(LEONOS_FETCH_SCRIPT),$(RELIEFOS_SRC)/tools/build/fetch.sh)
RELIEFOS_QEMU_IDE ?= $(LEONOS_QEMU_IDE)
RELIEFOS_QEMU_NVME ?= $(LEONOS_QEMU_NVME)

# Older wrappers may still pass the lock ownership token under its former
# name. The ReliefOS spelling wins whenever both are supplied.
ifeq ($(origin RELIEFOS_BUILD_OWNER),undefined)
ifneq ($(strip $(LEONOS_BUILD_OWNER)),)
RELIEFOS_BUILD_OWNER := $(LEONOS_BUILD_OWNER)
endif
endif
export RELIEFOS_BUILD_OWNER

# --- same-output-directory mutual exclusion ---------------------------------
# Two top-level makes sharing one O would race on objects, generated headers and
# command records, so one of them has to be refused outright (plan section 6.3).
# Different O directories are independent and may build concurrently.
#
# The owner is the make process that acquired the lock, and the token names the
# output directory it owns: RELIEFOS_BUILD_OWNER is "<pid>.<start ticks>:<absolute O>".
# A nested make for that same directory inherits it, so recursive build
# invocations do not refuse their own outer build. A nested
# make for a different directory acquires its own lock, so a test suite that
# spawns builds still gets real exclusion for the trees it creates.
#
# Three cases skip acquisition: dry runs and `make -q` (they promise no output,
# and refusing them would make `make -n` depend on unrelated builds), and goals
# that write nothing at all -- a bare `make` and `make help` must keep working
# while a build is running elsewhere, and must not create the output tree.
reliefos_read_only_goals := help doctor
reliefos_lock_not_needed :=
ifeq ($(MAKECMDGOALS),)
reliefos_lock_not_needed := 1
else ifeq ($(words $(filter $(reliefos_read_only_goals),$(MAKECMDGOALS))),$(words $(MAKECMDGOALS)))
reliefos_lock_not_needed := 1
endif
reliefos_lock_dir := $(O)/.build-lock
reliefos_lock_skip := \
	$(if $(findstring n,$(firstword -$(MAKEFLAGS))),1)\
	$(if $(findstring q,$(firstword -$(MAKEFLAGS))),1)\
	$(reliefos_lock_not_needed)
ifeq ($(strip $(reliefos_lock_skip)),)
RELIEFOS_BUILD_OWNER := $(shell sh $(RELIEFOS_SRC)/scripts/build-lock.sh acquire \
	$(reliefos_lock_dir) $(reliefos_O_absolute))
ifeq ($(RELIEFOS_BUILD_OWNER),)
$(error refusing to build: '$(O)' is already being built by another make)
endif
export RELIEFOS_BUILD_OWNER
endif

# The source entry only validates and delegates. All production rules are Kbuild.
.DEFAULT_GOAL := help
ifeq ($(V),1)
Q :=
else
Q := @
endif
ifneq ($(filter doctor,$(MAKECMDGOALS)),)
include $(RELIEFOS_SRC)/scripts/reliefos/Makefile.toolchain
endif
.PHONY: help doctor clean distclean htmldocs __kbuild
help:
	@V='$(V)' O='$(O)' ARCH='$(ARCH)' PROFILE='$(PROFILE)' CPUS='$(CPUS)' \
	TOOLCHAIN='$(TOOLCHAIN)' SRC='$(RELIEFOS_SRC)' CACHE='$(RELIEFOS_CACHE)' \
	LOCK='$(RELIEFOS_LOCK)' sh $(RELIEFOS_SRC)/scripts/help.sh

doctor:
	@SRC='$(RELIEFOS_SRC)' O='$(O)' ARCH='$(ARCH)' PROFILE='$(PROFILE)' \
	TOOLCHAIN='$(TOOLCHAIN)' HOSTCC='$(HOSTCC)' \
	TARGET_CC='$(TARGET_CC)' TARGET_LD='$(TARGET_LD)' TARGET_AR='$(TARGET_AR)' \
	TARGET_OBJCOPY='$(TARGET_OBJCOPY)' TARGET_STRIP='$(TARGET_STRIP)' \
	TARGET_TRIPLE_KERNEL='$(TRIPLE_KERNEL)' \
	TARGET_TRIPLE_USER='$(TRIPLE_USER)' \
	DEPS='$(RELIEFOS_DEPS_TOOL)' LOCK='$(RELIEFOS_LOCK)' CACHE='$(RELIEFOS_CACHE)' \
	sh $(RELIEFOS_SRC)/scripts/doctor.sh

clean:
	@O='$(reliefos_O_absolute)' SRC='$(RELIEFOS_SRC)' KEEP_CONFIG=1 sh $(RELIEFOS_SRC)/scripts/clean.sh

distclean:
	@O='$(reliefos_O_absolute)' SRC='$(RELIEFOS_SRC)' KEEP_CONFIG=0 sh $(RELIEFOS_SRC)/scripts/clean.sh

SPHINXBUILD ?= sphinx-build
SPHINXOPTS ?=
$(RELIEFOS_O_MARKER):
	$(Q)mkdir -p $(O)
	$(Q)printf 'reliefos-build-out version=1 root=%s\n' '$(RELIEFOS_SRC)' > $@.tmp
	$(Q)mv -f $@.tmp $@

htmldocs: $(RELIEFOS_O_MARKER)
	$(Q)SPHINXBUILD='$(SPHINXBUILD)' SPHINXOPTS='$(SPHINXOPTS)' \
	    sh $(RELIEFOS_SRC)/scripts/build-docs.sh $(RELIEFOS_SRC)/Documentation $(O)/Documentation/html

reliefos_goals := $(filter-out help doctor clean distclean htmldocs,$(MAKECMDGOALS))
reliefos_kbuild_goals := $(patsubst $(reliefos_O_absolute)/%,%,$(patsubst $(O)/%,%,$(reliefos_goals)))
ifneq ($(strip $(reliefos_goals)),)
.PHONY: $(reliefos_goals)
$(reliefos_goals): __kbuild ;
endif

# Inspection never creates O. Configuration-independent goals may inspect an
# existing host-only tree without creating a .config as a side effect.
reliefos_config_free_goals := tools fetch fetch-check reliefnt-fetch ntclks-fetch reliefnt-fetch-check ntclks-fetch-check headers_install defconfig olddefconfig menuconfig migrate-config regen-component-kconfig
ifneq ($(strip $(reliefos_lock_skip)),)
ifneq ($(strip $(reliefos_goals)),)
ifeq ($(wildcard $(reliefos_O_absolute)),)
$(error initialize this output with make O=$(O) defconfig before -n/-q)
endif
ifneq ($(strip $(filter-out $(reliefos_config_free_goals),$(reliefos_goals))),)
ifeq ($(wildcard $(reliefos_O_absolute)/.config),)
$(error initialize this output with make O=$(O) defconfig before -n/-q)
endif
reliefos_component_goals := all userland sdk apk-repo rootfs rootfs-raw image-vmdk iso installer \
	image-iso release test-smoke run run-debug run-iso run-installer rpr-apps rpr-pages
reliefos_selection_goals := $(filter $(reliefos_component_goals),$(reliefos_goals)) \
	$(filter app-% userland/apps/%,$(reliefos_goals))
ifneq ($(strip $(reliefos_selection_goals)),)
ifeq ($(wildcard $(reliefos_O_absolute)/config/components.mk),)
$(error configuration selection is incomplete at '$(reliefos_O_absolute)/config/components.mk'; run 'make O=$(O) config-sync' before -n/-q)
endif
endif
endif
endif
endif

# HOSTCC is a whitespace-separated command with optional wrapper/driver words.
# Keep its syntax shell-safe before interpolating the words into the native
# command, then hash every command word that resolves to a file so launcher and
# wrapped-driver replacements both invalidate .cmd records.
reliefos_hostcc_command := $(if $(strip $(HOSTCC)),$(HOSTCC),cc)
ifneq ($(findstring $(reliefos_comma),$(reliefos_hostcc_command)),)
$(error HOSTCC does not support comma characters; use simple command words and arguments)
endif
reliefos_hostcc_residual := $(call strip_allowed,$(subst $(reliefos_space),,$(reliefos_hostcc_command)),$(RELIEFOS_TOOL_SAFE_CHARS))
ifneq ($(strip $(reliefos_hostcc_residual)),)
$(error HOSTCC contains unsupported shell characters: [$(reliefos_hostcc_residual)]; use simple command words and arguments)
endif
# Include the actual host compiler identity in native saved commands. This
# covers in-place tool replacement even when the command path and flags match.
# It is evaluated only for build delegation, never by help/doctor.
reliefos_hostcc_fingerprint = $(shell sh $(RELIEFOS_SRC)/scripts/reliefos/hostcc-identity.sh $(reliefos_hostcc_command))
reliefos_native_hostcc = env RELIEFOS_HOST_COMPILER_ID=$(reliefos_hostcc_fingerprint) $(reliefos_hostcc_command)

__kbuild:
	$(Q)mkdir -p '$(reliefos_O_absolute)'
	$(Q)test -f '$(RELIEFOS_O_MARKER)' || printf 'reliefos-build-out version=1 root=%s\n' '$(RELIEFOS_SRC)' > '$(RELIEFOS_O_MARKER)'
	+$(Q)CONFIG_SHELL=/bin/sh AWK=awk srctree='$(RELIEFOS_SRC)' objtree=. VPATH='$(RELIEFOS_SRC)' \
	    $(MAKE) --no-print-directory -rR -C '$(reliefos_O_absolute)' \
	    -f '$(RELIEFOS_SRC)/scripts/Makefile.port' -f '$(RELIEFOS_SRC)/scripts/Makefile.build' \
	    obj=scripts/basic KBUILD_HOSTCFLAGS='-O2 -I$(RELIEFOS_SRC)/scripts/include' \
	    HOSTCC='$(reliefos_native_hostcc)'
	+$(Q)CONFIG_SHELL=/bin/sh AWK=awk srctree='$(RELIEFOS_SRC)' objtree=. VPATH='$(RELIEFOS_SRC)' \
	    RELIEFOS_SRC='$(RELIEFOS_SRC)' ARCH='$(ARCH)' PROFILE='$(PROFILE)' \
	    SOURCE_DATE_EPOCH='$(SOURCE_DATE_EPOCH)' TOOLCHAIN='$(TOOLCHAIN)' \
	    $(MAKE) --no-print-directory -rR -C '$(reliefos_O_absolute)' \
	    -f '$(RELIEFOS_SRC)/scripts/Makefile.port' -f '$(RELIEFOS_SRC)/scripts/Makefile.build' \
	    obj=. KBUILD_BUILTIN=1 HOSTCC='$(reliefos_native_hostcc)' $(reliefos_kbuild_goals)

# The source Makefile itself is never an implicit product.
$(firstword $(MAKEFILE_LIST)): ;
