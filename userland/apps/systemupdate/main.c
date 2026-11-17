/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cancelled(int signal_number)
{
    update_cancelled = signal_number;
}

static void usage(void)
{
    fputs("Usage: reliefos-update [--root ABSOLUTE_ROOT] check|prepare [--machine]\n"
          "       reliefos-update [--root ABSOLUTE_ROOT] apply --plan ID [--machine]\n"
          "       reliefos-update [--root ABSOLUTE_ROOT] status|resume|repair|discard|probe|configure [--machine]\n", stderr);
}

int main(int argc, char **argv)
{
    umask(077);
    struct update_context context = {.lock = -1};
    const char *root = "/", *command = NULL, *plan = NULL;
    const char *program = strrchr(argv[0], '/'); program = program ? program + 1 : argv[0];
    if (strstr(program, "rpr-ping")) command = "probe";
    else if (strstr(program, "rpr-apkcheck")) command = "configure";
    else if (strstr(program, "check-update")) command = "check";
    else if (strstr(program, "kernel-update")) {
        if (argc == 2 && !strcmp(argv[1], "--check")) { command = "check"; argc = 1; }
        else {
            fputs("The kernel update entry is now managed by reliefos-update. Run prepare, review its plan, then apply --plan ID.\n", stderr);
            return 2;
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "--machine")) context.machine = 1;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--plan") && i + 1 < argc && !plan) plan = argv[++i];
        else if (!command && argv[i][0] != '-') command = argv[i];
        else { usage(); return 2; }
    }
    if (!command) { usage(); return 2; }
    int guard = !strcmp(command, "pre-commit") || !strcmp(command, "post-commit");
    if ((!strcmp(command, "apply") && !update_token(plan)) || (plan && strcmp(command, "apply"))) { usage(); return 2; }
    int writing = strcmp(command, "status") && !guard;
    int result = update_context_init(&context, root, writing);
    if (result) { update_error("open managed ReliefOS update root", result); return result == -EPERM ? 77 : 1; }
    struct sigaction action = {.sa_handler = cancelled}; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL); sigaction(SIGHUP, &action, NULL);
    if (!strcmp(command, "check") || !strcmp(command, "prepare")) result = update_prepare(&context, 0);
    else if (!strcmp(command, "apply")) {
        result = update_plan_open(&context, plan);
        if (!result) result = update_apply(&context, 0);
    } else if (!strcmp(command, "status")) result = update_status(&context);
    else if (!strcmp(command, "resume") || !strcmp(command, "repair")) {
        result = update_active(&context, 0);
        if (!result) result = update_state_load(&context);
        if (!result) {
            if (!strcmp(command, "repair")) result = update_apply(&context, 1);
            else if (!strcmp(context.state.phase, "ready")) result = update_prepare(&context, 1);
            else if (!strcmp(context.state.phase, "complete")) result = update_apply(&context, 0);
            else if (!strcmp(context.state.phase, "committing") || !strcmp(context.state.phase, "apply-failed") ||
                     !strcmp(context.state.phase, "repairing") || !strcmp(context.state.phase, "verify-failed")) {
                fputs("[reliefos-update] interrupted package commit requires explicit repair; inspect status first\n", stderr);
                result = -EINPROGRESS;
            } else result = update_prepare(&context, 1);
        }
    } else if (!strcmp(command, "configure")) result = update_configure(&context);
    else if (!strcmp(command, "discard")) result = update_discard(&context);
    else if (!strcmp(command, "probe")) result = update_probe(&context);
    else if (!strcmp(command, "pre-commit")) result = update_guard(&context);
    else if (!strcmp(command, "post-commit")) result = 0;
    else { usage(); result = -EINVAL; }
    if (context.lock >= 0) close(context.lock);
    if (result) {
        update_error(command, result);
        if (context.machine) puts("result=failed");
    }
    return result ? update_cancelled ? 130 : 1 : 0;
}
