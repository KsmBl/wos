/* COM1 serial port, used as a debug log channel.
 *
 * QEMU's `-serial stdio` pipes this straight to the terminal, which makes it
 * far more useful than the VGA console for capturing boot output.
 */
#ifndef WOS_SERIAL_H
#define WOS_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t len);
bool serial_has_input(void);
char serial_getc(void);

#endif /* WOS_SERIAL_H */
