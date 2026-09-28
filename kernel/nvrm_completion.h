#ifndef KESTREL_NVRM_COMPLETION_H
#define KESTREL_NVRM_COMPLETION_H
#include "kernel.h"
/* Host RM owns and services engine interrupt leaves. This is routing readiness,
 * not evidence that a particular engine/job has ever signalled completion. */
bool nvrm_completion_irq_ready(void);
#endif
