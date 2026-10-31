/* Project policy derived from the native Kconfig header; no second parser. */
#ifndef RELIEFOS_BUILD_POLICY_H
#define RELIEFOS_BUILD_POLICY_H
#include <generated/autoconf.h>
#ifndef RELIEFOS_LICENSE_REQUIRE
#if defined(RELIEFOS_INSTALLER)
#if defined(CONFIG_INSTALLER_INSTALLED_REQUIRE_LICENSE) && CONFIG_INSTALLER_INSTALLED_REQUIRE_LICENSE
#define RELIEFOS_LICENSE_REQUIRE 1
#else
#define RELIEFOS_LICENSE_REQUIRE 0
#endif
#else
#if defined(CONFIG_VMDK_REQUIRE_LICENSE) && CONFIG_VMDK_REQUIRE_LICENSE
#define RELIEFOS_LICENSE_REQUIRE 1
#else
#define RELIEFOS_LICENSE_REQUIRE 0
#endif
#endif
#endif
#endif
