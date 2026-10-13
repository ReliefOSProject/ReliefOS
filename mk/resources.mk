# The CJK font generation (cjk_font.h, its unifont dependency closure and the
# framebuffer.c.o prerequisite) belongs to the kernel checkout since phase 3:
# the only consumer was drivers/video/framebuffer/framebuffer.c, which is built there.
RELIEFOS_FONT_TOOL := $(RELIEFOS_HOST_BIN)/reliefos-font
RELIEFOS_GRUB_FONT_TOOL := $(RELIEFOS_HOST_BIN)/reliefos-grub-font
$(RELIEFOS_FONT_TOOL): $(O_HOST)/obj/tools/host/assets/reliefos-font.c.o $(RELIEFOS_HOST_COMMON_OBJS)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@
$(RELIEFOS_GRUB_FONT_TOOL): $(O_HOST)/obj/tools/host/assets/reliefos-grub-font.c.o $(RELIEFOS_HOST_COMMON_OBJS)
	$(Q)$(HOSTCC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@
UI_METRO_FONT := $(O_GENERATED)/fonts/leonos-metro.ttf
UI_WIN95_FONT := $(O_GENERATED)/fonts/leonos-win95.ttf
GRUB_BDF := $(O_GENERATED)/grub/leonos-pixel.bdf
GRUB_FONT := $(O_GENERATED)/grub/leonos-unicode.pf2
$(UI_METRO_FONT) $(UI_WIN95_FONT) &: $(RELIEFOS_FONT_TOOL) $(RELIEFOS_SRC)/system/fonts/Deng.ttf $(RELIEFOS_SRC)/system/fonts/system.psf
	$(Q)mkdir -p $(@D)
	$(Q)$(RELIEFOS_FONT_TOOL) $(RELIEFOS_SRC)/system/fonts/Deng.ttf $(RELIEFOS_SRC)/system/fonts/system.psf $(UI_METRO_FONT) $(UI_WIN95_FONT)
$(GRUB_BDF): $(RELIEFOS_GRUB_FONT_TOOL) $(RELIEFOS_SRC)/system/fonts/system.psf
	$(Q)mkdir -p $(@D)
	$(Q)$(RELIEFOS_GRUB_FONT_TOOL) $(RELIEFOS_SRC)/system/fonts/system.psf $@
$(GRUB_FONT): $(GRUB_BDF)
	$(Q)grub-mkfont -s 16 -o $@.tmp $<
	$(Q)mv $@.tmp $@
COMPONENT_METADATA := $(O_CONFIG)/components.tsv
$(COMPONENT_METADATA): $(RELIEFOS_COMPONENT_TOOL) $(RELIEFOS_COMPONENT_MK) $(RELIEFOS_CONFIG_FILE)
	$(Q)$(RELIEFOS_COMPONENT_TOOL) --input $(RELIEFOS_SRC)/configs/components.toml --config $(RELIEFOS_CONFIG_FILE) --output $(RELIEFOS_COMPONENT_MK) --metadata $@
.PHONY: resources
resources: $(UI_METRO_FONT) $(UI_WIN95_FONT) $(GRUB_FONT) $(COMPONENT_METADATA)
tools: $(RELIEFOS_FONT_TOOL) $(RELIEFOS_GRUB_FONT_TOOL) $(RELIEFOS_COMPONENT_TOOL) $(RELIEFOS_GEARS_TOOL)
