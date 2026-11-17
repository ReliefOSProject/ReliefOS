/* SPDX-License-Identifier: Apache-2.0 */
#include <sys/utsname.h>
#include <stdlib.h>
#include <string.h>

/** @brief Admit only the private-root CLI regression as a simulated ReliefNT boot. */
int __wrap_uname(struct utsname *system)
{
    const char *release = getenv("RELIEFOS_TEST_UNAME_RELEASE");
    if (!release || strlen(release) >= sizeof(system->release)) return -1;
    memset(system, 0, sizeof(*system));
    strcpy(system->sysname, "ReliefNT");
    strcpy(system->release, release);
    return 0;
}
