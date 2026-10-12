# Third-party components: the dependency lock file, the download cache, and the
# upstream sources the userland links against.
#
# Nothing here decides *how* an upstream project builds. Each component is
# configured and compiled by its own upstream build system, driven by a short
# script in tools/build; this fragment only says which pinned source is used,
# which products prove it worked, and when the work has to be redone.

RELIEFOS_LOCK ?= $(if $(strip $(LEONOS_LOCK)),$(LEONOS_LOCK),$(RELIEFOS_SRC)/configs/dependencies.lock.json)
RELIEFOS_FETCH_SCRIPT ?= $(if $(strip $(LEONOS_FETCH_SCRIPT)),$(LEONOS_FETCH_SCRIPT),$(RELIEFOS_SRC)/tools/build/fetch.sh)

# --- the lock file must describe what it claims -------------------------------
# Parse-time, but read-only: `make fetch` and every adapter consult the lock
# through reliefos-deps, and a broken file has to stop the build before anything
# is downloaded.
$(RELIEFOS_CACHE):
	$(Q)mkdir -p $@

.PHONY: reliefos-check-lock leonos-check-lock
reliefos-check-lock: | $(RELIEFOS_O_MARKER) $(RELIEFOS_DEPS_TOOL)
	$(call RELIEFOS_LOG,CHECK,$(RELIEFOS_LOCK))
	$(Q)$(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_LOCK) --check --root $(RELIEFOS_SRC)
leonos-check-lock: reliefos-check-lock

fetch: reliefos-check-lock reliefnt-fetch | $(RELIEFOS_CACHE)
	$(call RELIEFOS_LOG,FETCH,$(RELIEFOS_CACHE))
	$(Q)sh $(RELIEFOS_FETCH_SCRIPT) --deps $(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_LOCK) \
		--cache $(RELIEFOS_CACHE)

# URL liveness for everything `make fetch` would download: a 404 or a vanished
# mirror is named here before a build ever has to refuse for missing cache
# bytes.  The kernel checkout's locked URLs are probed the same way (see
# reliefnt-fetch-check in mk/kernel.mk).  Probes only: nothing is downloaded.
.PHONY: fetch-check
fetch-check: reliefos-check-lock reliefnt-fetch-check
	$(call RELIEFOS_LOG,FETCH-CHECK,$(RELIEFOS_LOCK))
	$(Q)sh $(RELIEFOS_SRC)/tools/build/fetch-check.sh --deps $(RELIEFOS_DEPS_TOOL) \
		--lock $(RELIEFOS_LOCK)

# Used by the build itself: a dependency that was never fetched stops the build
# with the id and the command that fixes it, instead of silently going online
# (plan section 9).
.PHONY: reliefos-verify-cache leonos-verify-cache
reliefos-verify-cache: | $(RELIEFOS_DEPS_TOOL)
	$(Q)sh $(RELIEFOS_FETCH_SCRIPT) --deps $(RELIEFOS_DEPS_TOOL) --lock $(RELIEFOS_LOCK) \
		--cache $(RELIEFOS_CACHE) --verify-only
leonos-verify-cache: reliefos-verify-cache

# --- musl and mimalloc --------------------------------------------------------
MUSL_SYSROOT := $(O_SYSROOT)/musl
MUSL_STAMP := $(MUSL_SYSROOT)/.leonos-musl.json
MUSL_WORK := $(O_THIRD_PARTY)/musl
MUSL_SCRIPT := $(RELIEFOS_SRC)/tools/build/musl-sysroot.sh

# musl's own configure probes with the compiler word, so the target triple has
# to stay inside CC; these are the flags the retired Python driver passed.
RELIEFOS_MUSL_CFLAGS := -O2 -fno-stack-protector -mno-avx

# The products the rest of the build links against. Declaring them makes an
# install that "succeeded" while leaving something out a Make error rather than
# a link failure three stages later.
RELIEFOS_MUSL_ARTIFACTS := \
	$(MUSL_SYSROOT)/lib/libc.so \
	$(MUSL_SYSROOT)/lib/libc.a \
	$(MUSL_SYSROOT)/lib/libmimalloc.so.3 \
	$(MUSL_SYSROOT)/lib/mimalloc.o \
	$(MUSL_SYSROOT)/lib/Scrt1.o \
	$(MUSL_SYSROOT)/lib/crt1.o \
	$(MUSL_SYSROOT)/lib/crti.o \
	$(MUSL_SYSROOT)/lib/crtn.o \
	$(MUSL_SYSROOT)/lib/rcrt1.o \
	$(MUSL_SYSROOT)/lib/libssp_nonshared.a \
	$(MUSL_SYSROOT)/include/stdio.h \
	$(MUSL_SYSROOT)/include/mimalloc.h \
	$(MUSL_SYSROOT)/share/licenses/musl/COPYRIGHT \
	$(MUSL_SYSROOT)/share/licenses/mimalloc/LICENSE

# The whole lock file digest is part of the signature rather than just the musl
# entries. That is deliberate: reading a subset here would need reliefos-deps to
# exist before it has been built, and this project does not parse JSON with sed.
# The cost is one extra sysroot rebuild when an unrelated dependency is edited.
RELIEFOS_LOCK_DIGEST := $(if $(RELIEFOS_PASSIVE),unavailable,\
	$(shell sha256sum $(RELIEFOS_LOCK) 2>/dev/null | cut -d' ' -f1))
# git is part of the contract: the pinned trees are submodule checkouts and the
# adapter verifies their HEAD before unpacking.
reliefos_git_path := $(if $(RELIEFOS_PASSIVE),deferred,$(shell command -v git 2>/dev/null || echo unavailable))

RELIEFOS_SIG_musl-sysroot := script=$(MUSL_SCRIPT)|lock=$(RELIEFOS_LOCK_DIGEST)|cc=$(TARGET_CC)|ar=$(TARGET_AR)|ranlib=$(TARGET_RANLIB)|ld=$(TARGET_LD)|triple=$(TRIPLE_USER)|cflags=$(RELIEFOS_MUSL_CFLAGS)|git=$(reliefos_git_path)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,musl-sysroot)))

$(MUSL_STAMP) $(RELIEFOS_MUSL_ARTIFACTS) &: $(RELIEFOS_LOCK) $(MUSL_SCRIPT) $(RELIEFOS_SHELL_LOG) $(RELIEFOS_DEPS_TOOL) $(O_META)/musl-sysroot.sig \
	| $(MUSL_SYSROOT) $(MUSL_WORK) $(O_LOGS)
	$(call RELIEFOS_LOG,SYSROOT,musl)
	+$(Q)case "$${MAKEFLAGS%% *}" in *n*) exit 0;; esac; sh $(MUSL_SCRIPT) --src '$(RELIEFOS_SRC)' --deps '$(abspath $(RELIEFOS_DEPS_TOOL))' \
		--lock '$(RELIEFOS_LOCK)' --work '$(abspath $(MUSL_WORK))' --sysroot '$(abspath $(MUSL_SYSROOT))' \
		--cc '$(TARGET_CC)' --ar '$(TARGET_AR)' --ranlib '$(TARGET_RANLIB)' \
		--ld '$(TARGET_LD)' --target '$(TRIPLE_USER)' \
		--cflags '$(RELIEFOS_MUSL_CFLAGS)' --log $(abspath $(O_LOGS))/musl.log
	$(Q)for product in $(RELIEFOS_MUSL_ARTIFACTS); do test -e "$$product" || exit 1; touch "$$product"; done
	$(Q)touch $(MUSL_STAMP)

$(MUSL_SYSROOT) $(MUSL_WORK) $(O_THIRD_PARTY):
	$(Q)mkdir -p $@

.DELETE_ON_ERROR: $(MUSL_STAMP)

# --- authentication chain -----------------------------------------------------
# Only the packages that still ship a POSIX `configure` are built here:
# Linux-PAM 1.7 uses its dedicated Make port in pam.mk. This adapter stages:
# the Linux UAPI headers and libxcrypt, which is what `crypt()` in the userland
# extensions links against.
AUTH_ROOT := $(O_AUTH)/root
AUTH_STAMP := $(O_AUTH)/.leonos-auth.json
AUTH_WORK := $(O_THIRD_PARTY)/auth
AUTH_SCRIPT := $(RELIEFOS_SRC)/tools/build/auth-upstream.sh
RELIEFOS_AUTH_CFLAGS := -O2 -mno-avx -mno-avx2

# Declared, not implied by a stamp: an install that "succeeded" while leaving a
# header or a SONAME behind has to be a Make error here.
RELIEFOS_AUTH_ARTIFACTS := \
	$(AUTH_ROOT)/usr/include/crypt.h \
	$(AUTH_ROOT)/usr/include/linux/openat2.h \
	$(AUTH_ROOT)/lib/libcrypt.so.2 \
	$(AUTH_ROOT)/share/licenses/libxcrypt/LICENSE \
	$(AUTH_ROOT)/share/licenses/linux-headers/LICENSE

RELIEFOS_SIG_auth-upstream := script=$(AUTH_SCRIPT)|lock=$(RELIEFOS_LOCK_DIGEST)|cc=$(TARGET_CC)|ar=$(TARGET_AR)|ranlib=$(TARGET_RANLIB)|triple=$(TRIPLE_USER)|cflags=$(RELIEFOS_AUTH_CFLAGS)|resource=$(shell $(TARGET_CC) -print-resource-dir 2>/dev/null)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,auth-upstream)))

.PHONY: reliefos-auth leonos-auth
reliefos-auth: $(AUTH_STAMP) $(RELIEFOS_AUTH_ARTIFACTS)
leonos-auth: reliefos-auth

$(AUTH_STAMP) $(RELIEFOS_AUTH_ARTIFACTS) &: $(RELIEFOS_LOCK) $(AUTH_SCRIPT) $(RELIEFOS_SHELL_LOG) $(RELIEFOS_DEPS_TOOL) \
	$(O_META)/auth-upstream.sig $(MUSL_STAMP) | $(O_AUTH) $(AUTH_WORK) $(O_LOGS)
	$(call RELIEFOS_LOG,AUTH,auth)
	+$(Q)case "$${MAKEFLAGS%% *}" in *n*) exit 0;; esac; sh $(AUTH_SCRIPT) --src '$(RELIEFOS_SRC)' --deps '$(abspath $(RELIEFOS_DEPS_TOOL))' \
		--lock '$(RELIEFOS_LOCK)' --cache '$(RELIEFOS_CACHE)' --work '$(abspath $(AUTH_WORK))' \
		--stage '$(abspath $(AUTH_ROOT))' --sysroot '$(abspath $(MUSL_SYSROOT))' \
		--cc '$(TARGET_CC)' --ar '$(TARGET_AR)' --ranlib '$(TARGET_RANLIB)' \
		--target '$(TRIPLE_USER)' --cflags '$(RELIEFOS_AUTH_CFLAGS)' \
		--log $(abspath $(O_LOGS))/auth.log
	$(Q)cp $(AUTH_WORK)/.leonos-auth.json $(AUTH_STAMP).tmp
	$(Q)mv $(AUTH_STAMP).tmp $(AUTH_STAMP)
	$(Q)for product in $(RELIEFOS_AUTH_ARTIFACTS); do test -e "$$product" || exit 1; touch "$$product"; done

$(O_AUTH) $(AUTH_WORK):
	$(Q)mkdir -p $@

.DELETE_ON_ERROR: $(AUTH_STAMP)
