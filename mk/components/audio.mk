# Audio is opt-in for applications; the base runtime has no libasound dependency.
AUDIO_ALSA_ROOT := $(UPSTREAM_ROOT)/alsa-lib/root
AUDIO_OPL_ROOT := $(UPSTREAM_ROOT)/nuked-opl3/root
AUDIO_SDK_PRODUCTS := $(upstream_alsa-lib_products) $(upstream_nuked-opl3_products)

AUDIO_CONTROL_SO := $(RUNTIME_DIR)/libreliefos-audio.so.1
AUDIO_OBJECTS := $(addprefix $(O_OBJ)/audio/,audio_control.o audio_test.o)
AUDIO_FLAGS = $(RUNTIME_FLAGS) -nostdinc -isystem $(if $(RELIEFOS_PASSIVE),deferred,$(shell $(TARGET_CC) -print-resource-dir 2>/dev/null))/include -D_POSIX_C_SOURCE=200809L -I$(AUDIO_ALSA_ROOT)/usr/include
RELIEFOS_SIG_audio-cc := cc=$(TARGET_CC)|flags=$(AUDIO_FLAGS) $(USERLAND_CFLAGS)
RELIEFOS_SIG_audio-ld := ld=$(TARGET_LD)|policy=$(RELIEFOS_LINK_POLICY_FLAGS)|objects=$(AUDIO_OBJECTS)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,audio-cc)))
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,audio-ld)))

$(O_OBJ)/audio/%.o: $(RELIEFOS_SRC)/userland/audio/%.c $(MUSL_STAMP) $(HEADER_EXPORT_MANIFEST) $(upstream_alsa-lib_products) $(O_META)/audio-cc.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,CC,$<)
	$(Q)$(TARGET_CC) $(AUDIO_FLAGS) $(USERLAND_CFLAGS) -MMD -MP -MF $@.d -MT $@ -c $< -o $@.tmp
	$(Q)mv $@.tmp $@

$(AUDIO_CONTROL_SO): $(AUDIO_OBJECTS) $(AUDIO_ALSA_ROOT)/usr/lib/libasound.so.2 $(MUSL_SYSROOT)/lib/libc.so $(RUNTIME_BUILTINS) $(O_META)/audio-ld.sig
	$(Q)mkdir -p $(dir $@)
	$(call RELIEFOS_LOG,LD,$@)
	$(Q)$(TARGET_LD) $(RELIEFOS_LINK_POLICY_FLAGS) -shared --no-undefined --hash-style=both -z max-page-size=0x1000 -soname libreliefos-audio.so.1 -o $@.tmp $(AUDIO_OBJECTS) $(AUDIO_ALSA_ROOT)/usr/lib/libasound.so.2 -L$(MUSL_SYSROOT)/lib -lc $(RUNTIME_BUILTINS)
	$(Q)mv $@.tmp $@

AUDIO_SDK_PRODUCTS += $(AUDIO_CONTROL_SO)
USERLAND_EXTRA_soundctl = -I$(AUDIO_ALSA_ROOT)/usr/include
USERLAND_LIBS_soundctl = $(AUDIO_CONTROL_SO) $(AUDIO_ALSA_ROOT)/usr/lib/libasound.so.2 $(RUNTIME_BUILTINS)
USERLAND_DEPS_soundctl = $(upstream_alsa-lib_products)
USERLAND_LIBS_settings = $(AUDIO_CONTROL_SO) $(AUDIO_ALSA_ROOT)/usr/lib/libasound.so.2 $(RUNTIME_BUILTINS)
USERLAND_DEPS_settings = $(upstream_alsa-lib_products)
-include $(addsuffix .d,$(AUDIO_OBJECTS))
