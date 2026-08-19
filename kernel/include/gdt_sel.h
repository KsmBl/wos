/* Segment selectors, in a form assembly can include too.
 *
 * Kept apart from gdt.h so that sysentry.S can name the same constants the C
 * side uses.  The values are not free: SYSCALL and SYSRET derive CS and SS
 * from a single base held in the STAR register, which fixes their order in the
 * table.  See sysentry.c, which checks the arithmetic still works out.
 */
#ifndef WOS_GDT_SEL_H
#define WOS_GDT_SEL_H

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1B      /* 0x18 | RPL 3 */
#define SEL_UCODE 0x23      /* 0x20 | RPL 3 */
#define SEL_TSS   0x28

#endif /* WOS_GDT_SEL_H */
