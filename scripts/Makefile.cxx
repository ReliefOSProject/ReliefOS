# SPDX-License-Identifier: GPL-2.0
# ReliefOS C++ object action.  The dependency file and saved command are owned
# by the upstream Kbuild macros; this file only supplies the missing language
# action for .cpp inputs.

cxx_flags = -Wp,-MMD,$(depfile) $(KBUILD_CPPFLAGS) $(KBUILD_CXXFLAGS) \
	$(cxxflags-y) $(CXXFLAGS_$(target-stem).o)

quiet_cmd_cxx_o_cpp = CXX $(quiet_modtag) $@
      cmd_cxx_o_cpp = $(TARGET_CXX) $(cxx_flags) -c -o $@ $<

$(obj)/%.o: $(src)/%.cpp $(RELIEFOS_CXX_ID) FORCE
	$(call if_changed_dep,cxx_o_cpp)
