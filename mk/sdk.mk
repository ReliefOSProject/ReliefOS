# Relocatable musl SDK subset (the developer kit assembled from build output).
MUSL_SDK := $(O)/sdk/reliefos-musl-sdk
MUSL_SDK_ARCHIVE := $(O_PACKAGES)/reliefos-musl-sdk.tar.gz
SDK_EPOCH := $(or $(SOURCE_DATE_EPOCH),$(shell git -C $(RELIEFOS_SRC) show -s --format=%ct HEAD))
SDK_INPUT_HEADERS := $(shell find $(RELIEFNT_DIR)/include/uapi $(RELIEFOS_SRC)/include/leonos $(RELIEFOS_SRC)/include/reliefos $(RELIEFOS_SRC)/userland/runtime/include/leonos $(RELIEFOS_SRC)/userland/runtime/include/reliefos -type f \( -name '*.h' -o -name '*.inc' \) | LC_ALL=C sort)
RELIEFOS_SIG_sdk := epoch=$(SDK_EPOCH)|headers=$(SDK_INPUT_HEADERS)|driver=$(RELIEFOS_SDK_DRIVER)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,sdk)))

$(COMPONENT_SELECTION): $(O_CONFIG)/components.mk $(RELIEFOS_CONFIG_FILE) $(RELIEFOS_SRC)/configs/components.toml $(RELIEFOS_COMPONENT_TOOL)
	$(Q)set -eu; mkdir -p $(dir $@)
	$(Q)set -eu; $(O_HOST)/bin/reliefos-components --input $(RELIEFOS_SRC)/configs/components.toml \
	 --config $(RELIEFOS_CONFIG_FILE) --output $(O_CONFIG)/components.mk --selection $@

MUSL_SDK_REQUIRED := $(MUSL_SDK)/bin/reliefos-musl-cc $(MUSL_SDK)/include/stdio.h \
 $(MUSL_SDK)/include/pnglibconf.h $(MUSL_SDK)/lib/crt1.o $(MUSL_SDK)/lib/crti.o \
 $(MUSL_SDK)/lib/crtn.o $(MUSL_SDK)/lib/libc.a $(MUSL_SDK)/lib/libc.so \
 $(MUSL_SDK)/lib/libreliefos.so.2 $(MUSL_SDK)/lib/libleonos.so.2 \
 $(MUSL_SDK)/lib/libreliefos.a $(MUSL_SDK)/lib/libleonos.a \
 $(MUSL_SDK)/include/alsa/asoundlib.h $(MUSL_SDK)/include/opl3.h \
 $(MUSL_SDK)/lib/libasound.so.2 $(MUSL_SDK)/lib/libasound.a \
 $(MUSL_SDK)/lib/libopl3.so.1 $(MUSL_SDK)/share/alsa/alsa.conf \
 $(MUSL_SDK)/lib/libreliefos-audio.so.1 $(MUSL_SDK)/include/reliefos/audio_control.h
$(MUSL_SDK_REQUIRED) &: $(RUNTIME_SO) $(RUNTIME_COMPAT_SO) $(RUNTIME_ARCHIVE) $(RUNTIME_COMPAT_ARCHIVE) $(RUNTIME_BUILTINS) \
 $(MUSL_STAMP) $(RELIEFOS_MUSL_ARTIFACTS) $(PAM_STAMP) $(AUTH_STAMP) $(SDK_INPUT_HEADERS) \
 $(HEADER_EXPORT_MANIFEST) $(AUDIO_SDK_PRODUCTS) \
 $(PNG_CONFIG) $(RELIEFOS_SDK_DRIVER) $(O_META)/sdk.sig $(RELIEFOS_SRC)/tools/build/musl-sdk.sh
	$(Q)set -eu; AUDIO_CONTROL_SO=$(abspath $(AUDIO_CONTROL_SO)) AUDIO_ALSA_ROOT=$(abspath $(AUDIO_ALSA_ROOT)) AUDIO_OPL_ROOT=$(abspath $(AUDIO_OPL_ROOT)) RUNTIME_BUILTINS=$(RUNTIME_BUILTINS) PNG_CONFIG=$(PNG_CONFIG) sh $(RELIEFOS_SRC)/tools/build/musl-sdk.sh $(RELIEFOS_SRC) $(MUSL_SYSROOT) $(RUNTIME_SO) $(RUNTIME_ARCHIVE) $(RUNTIME_COMPAT_SO) $(RUNTIME_COMPAT_ARCHIVE) $(PAM_ROOT) $(AUTH_ROOT) $(RELIEFOS_SDK_DRIVER) $(MUSL_SDK) $(SDK_EPOCH) $(HEADER_EXPORT_INCLUDE)
	$(Q)set -eu; for product in $(MUSL_SDK_REQUIRED); do test -f "$$product" || exit 1; touch "$$product"; done
	$(Q)set -eu; find $(MUSL_SDK) -mindepth 1 ! -type d -printf '%P\n' | LC_ALL=C sort >$(MUSL_SDK).files

$(MUSL_SDK_ARCHIVE): $(MUSL_SDK_REQUIRED) $(O_META)/sdk.sig
	$(Q)set -eu; mkdir -p $(dir $@)
	$(Q)set -eu; tar --sort=name --mtime=@$(SDK_EPOCH) --owner=0 --group=0 --numeric-owner -cf $@.tar.tmp -C $(dir $(MUSL_SDK)) $(notdir $(MUSL_SDK))
	$(Q)set -eu; gzip -n -c $@.tar.tmp >$@.tmp
	$(Q)set -eu; mv $@.tmp $@
	$(Q)set -eu; rm $@.tar.tmp

.PHONY: musl-sdk sdk
musl-sdk: $(MUSL_SDK_ARCHIVE)
# `sdk` is the relocatable musl SDK built entirely from build output. The old
# checked-in devtools kit and the LeonOS4-Developer-SDK.zip are gone (user
# decision 2026-09-25); musl is the single C sysroot source.
sdk: $(MUSL_SDK_ARCHIVE)

# Every staged member participates in recovery, including non-primary headers,
# documentation and symlinks. Presence signatures are stable on intact trees.
define RELIEFOS_SDK_PRESENCE
$(O_META)/$(1)-present.sig: FORCE $(RELIEFOS_SRC)/tools/build/upstream-verify.sh
	$$(Q)sh $(RELIEFOS_SRC)/tools/build/upstream-verify.sh $(2) $(2).files $$@
$(3): $(O_META)/$(1)-present.sig
endef
$(eval $(call RELIEFOS_SDK_PRESENCE,musl-sdk,$(MUSL_SDK),$(MUSL_SDK_REQUIRED)))
