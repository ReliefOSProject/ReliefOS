/* SPDX-License-Identifier: Apache-2.0 */
#include "../../userland/apps/systemupdate/internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* Model a target refresh that incorporated a concurrent policy change. The
 * actual pre-commit guard must still bind the independently frozen policy. */
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    struct update_context context = {.lock = -1};
    int result = update_context_init(&context, argv[1], 0);
    if (!result) result = update_active(&context, 0);
    if (!result) result = update_state_load(&context);
    if (!result) result = update_target_hash(&context, context.state.target);
    if (!result) result = update_state_save(&context, "repairing");
    if (!result && setenv("RELIEFOS_UPDATE_PLAN", context.id, 1)) result = -errno;
    if (!result) result = update_guard(&context);
    if (result) fprintf(stderr, "policy guard probe result=%d\n", result);
    return result ? 1 : 0;
}
