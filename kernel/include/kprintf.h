/* Kernel formatted output.
 *
 * Everything printed here goes to both the VGA console and COM1, so a boot log
 * captured with `-serial stdio` is identical to what is on screen.
 *
 * Supported conversions:
 *   %d  signed decimal          %u  unsigned decimal
 *   %x  lowercase hex           %X  uppercase hex
 *   %p  pointer (0x00000000)    %c  character
 *   %s  NUL-terminated string   %%  a literal '%'
 * Width and zero padding are supported for the integer conversions, e.g. %08x.
 *
 * Byte counts are printed by passing fmt_bytes() to a %s, which keeps the
 * format-string checking honest:
 *     kprintf("free: %s\n", fmt_bytes(n));
 */
#ifndef WOS_KPRINTF_H
#define WOS_KPRINTF_H

#include "types.h"
#include <stdarg.h>

void kputc(char c);
void kputs(const char *s);

/* Format a byte count with a B/KiB/MiB/GiB suffix, e.g. "256 MiB".
 * Returns one of a small rotating set of static buffers, so up to four
 * results can be live in a single kprintf call. Not reentrant. */
const char *fmt_bytes(uint32_t bytes);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

/* Print a message and halt the machine.  Never returns. */
void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif /* WOS_KPRINTF_H */
