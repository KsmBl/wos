/* The SYSCALL entry path.  See sysentry.c. */
#ifndef WOS_SYSENTRY_H
#define WOS_SYSENTRY_H

#include "types.h"

/* Per-processor scratch for the entry stub.  The field order is fixed by
 * sysentry.S, which reaches both through GS. */
typedef struct {
    uint64_t kernel_rsp;    /* the stack to switch to on entry */
    uint64_t user_rsp;      /* where the caller's stack is kept meanwhile */
} sysentry_cpu_t;

/* Enable SYSCALL on the calling processor.  Every core must call it for
 * itself.  False on a CPU without the instruction, where int 0x80 remains the
 * only way in and everything still works. */
bool sysentry_init_cpu(void);

/* Tell this processor which stack a SYSCALL should land on.  Called from the
 * same place the TSS is pointed at a thread's kernel stack, and for the same
 * reason. */
void sysentry_set_kernel_stack(uint64_t rsp0);

#endif /* WOS_SYSENTRY_H */
