# ReliefMath has a separate C-API graphics development tree and process.
RELIEFMATH_DEV := $(O)/upstream/reliefmath-development
RELIEFMATH_DEV_MEMBERS := $(sort usr/include/cairo/cairo.h usr/include/freetype2/ft2build.h usr/lib/libcairo.so usr/lib/libcairo.so.2 usr/lib/libfreetype.so usr/lib/libfreetype.so.6 $(shell cat $(RELIEFMATH_DEV)/.members 2>/dev/null))
RELIEFMATH_DEV_PRODUCTS := $(RELIEFMATH_DEV)/.members $(addprefix $(RELIEFMATH_DEV)/,$(RELIEFMATH_DEV_MEMBERS))
$(RELIEFMATH_DEV)/.complete $(RELIEFMATH_DEV_PRODUCTS) &: $(RELIEFOS_SRC)/tools/build/reliefmath-development.sh $(RELIEFOS_LOCK) $(RELIEFOS_DEPS_TOOL) $(UPSTREAM_APK)
	$(Q)sh $(RELIEFOS_SRC)/tools/build/reliefmath-development.sh $(RELIEFOS_SRC) $(abspath $(RELIEFOS_DEPS_TOOL)) $(RELIEFOS_LOCK) $(RELIEFOS_CACHE) $(abspath $(UPSTREAM_APK)) $(abspath $(RELIEFMATH_DEV))
.PHONY: reliefmath-development
reliefmath-development: $(RELIEFMATH_DEV)/.complete $(RELIEFMATH_DEV_PRODUCTS)

RELIEFMATH_WORKER_CORE := model.cpp render_types.cpp protocol.cpp
RELIEFMATH_WORKER_SOURCES := $(sort $(patsubst $(RELIEFOS_SRC)/%,%,$(wildcard $(RELIEFOS_SRC)/userland/apps/reliefmath/worker/*.cpp)) $(addprefix userland/apps/reliefmath/,$(RELIEFMATH_WORKER_CORE)))
RELIEFMATH_WORKER_FLAGS := $(USERLAND_CXX_FLAGS) -iquote $(RELIEFOS_SRC)/userland/apps/reliefmath -isystem $(RELIEFMATH_DEV)/usr/include/cairo -isystem $(RELIEFMATH_DEV)/usr/include/freetype2
RELIEFMATH_WORKER_LIBS := $(RELIEFMATH_DEV)/usr/lib/libcairo.so $(RELIEFMATH_DEV)/usr/lib/libfreetype.so
RELIEFMATH_WORKER_OBJECTS := $(addprefix $(O_OBJ)/reliefmath-worker/,$(addsuffix .o,$(RELIEFMATH_WORKER_SOURCES)))
RELIEFOS_SIG_reliefmath-worker-cc := cxx=$(TARGET_CXX)|flags=$(RELIEFMATH_WORKER_FLAGS)|sources=$(RELIEFMATH_WORKER_SOURCES)
RELIEFOS_SIG_reliefmath-worker-ld := ld=$(TARGET_LD)|flags=$(USERLAND_LINK_FLAGS)|sources=$(RELIEFMATH_WORKER_SOURCES)|libs=$(RELIEFMATH_WORKER_LIBS)
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,reliefmath-worker-cc)))
$(if $(RELIEFOS_PASSIVE),,$(eval $(call RELIEFOS_SIGNATURE_RULE,reliefmath-worker-ld)))
$(O_OBJ)/reliefmath-worker/%.cpp.o: $(RELIEFOS_SRC)/%.cpp $(AUTOCONF_H) $(MUSL_STAMP) $(HEADER_EXPORT_MANIFEST) $(RELIEFMATH_DEV)/.complete $(RELIEFMATH_DEV_PRODUCTS) $(O_META)/reliefmath-worker-cc.sig
	$(Q)mkdir -p $(@D)
	$(call RELIEFOS_LOG,CXX,$<)
	$(Q)$(TARGET_CXX) $(RELIEFMATH_WORKER_FLAGS) -include $(AUTOCONF_H) -MMD -MP -MF $@.d -MT $@ -c $< -o $@.tmp
	$(Q)mv $@.tmp $@
$(USERLAND_DIR)/reliefmath-render.elf: $(RELIEFMATH_WORKER_OBJECTS) $(RELIEFMATH_WORKER_LIBS) $(USERLAND_CRT) $(MUSL_SYSROOT)/lib/libc.so $(MUSL_SYSROOT)/lib/libmimalloc.so.3 $(O_META)/reliefmath-worker-ld.sig
	$(Q)mkdir -p $(@D)
	$(call RELIEFOS_LOG,LD,$@)
	$(Q)$(TARGET_LD) $(USERLAND_LINK_FLAGS) -o $@.tmp $(MUSL_SYSROOT)/lib/Scrt1.o $(MUSL_SYSROOT)/lib/crti.o $(RELIEFMATH_WORKER_OBJECTS) -L$(MUSL_SYSROOT)/lib -l:libmimalloc.so.3 $(RELIEFMATH_WORKER_LIBS) -lc $(MUSL_SYSROOT)/lib/crtn.o
	$(Q)mv $@.tmp $@
.PHONY: app-reliefmath-render
app-reliefmath-render: $(USERLAND_DIR)/reliefmath-render.elf
USERLAND_DEPS_reliefmath += $(USERLAND_DIR)/reliefmath-render.elf

RELIEFMATH_PROBE_OBJ := $(O_OBJ)/reliefmath-worker/tools/tests/reliefmath_backend_probe.cpp.o
$(O)/tests/reliefmath-backend-probe.elf: $(RELIEFMATH_PROBE_OBJ) $(filter-out %/worker/main.cpp.o,$(RELIEFMATH_WORKER_OBJECTS)) $(RELIEFMATH_WORKER_LIBS) $(USERLAND_CRT)
	$(Q)mkdir -p $(@D)
	$(Q)$(TARGET_LD) $(USERLAND_LINK_FLAGS) -o $@.tmp $(MUSL_SYSROOT)/lib/Scrt1.o $(MUSL_SYSROOT)/lib/crti.o $(RELIEFMATH_PROBE_OBJ) $(filter-out %/worker/main.cpp.o,$(RELIEFMATH_WORKER_OBJECTS)) -L$(MUSL_SYSROOT)/lib -l:libmimalloc.so.3 $(RELIEFMATH_WORKER_LIBS) -lc $(MUSL_SYSROOT)/lib/crtn.o
	$(Q)mv $@.tmp $@
.PHONY: reliefmath-backend-probe
reliefmath-backend-probe: $(O)/tests/reliefmath-backend-probe.elf
-include $(addsuffix .d,$(RELIEFMATH_WORKER_OBJECTS)) $(RELIEFMATH_PROBE_OBJ).d
