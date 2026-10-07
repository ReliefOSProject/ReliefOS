#ifndef RELIEFOS_FILEMAN_MOTIF_H
#define RELIEFOS_FILEMAN_MOTIF_H
#include "fileman.h"
#include <Xm/Xm.h>

extern XtAppContext fileman_app;
extern Widget fileman_shell;
void fileman_motif_build(void);
void fileman_motif_refresh(void);
void fileman_motif_status(void);
void fileman_motif_settings(void);
void fileman_motif_action(uint32_t action);
int fileman_motif_wait(Widget dialog);
void fileman_motif_answer(Widget widget, XtPointer answer, XtPointer call);
void fileman_motif_label(Widget widget, const char *text);
#endif
