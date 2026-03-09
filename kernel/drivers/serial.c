/* 16550 UART driver for COM1, polled (no interrupts).
 *
 * Polling is fine here: serial output is only used for logging, and losing a
 * few microseconds per character during boot costs nothing.
 */

#include "serial.h"
#include "io.h"

#define COM1 0x3F8

#define REG_DATA        0   /* DLAB=0: data register        */
#define REG_INT_ENABLE  1   /* DLAB=0: interrupt enable     */
#define REG_DIVISOR_LO  0   /* DLAB=1: divisor low byte     */
#define REG_DIVISOR_HI  1   /* DLAB=1: divisor high byte    */
#define REG_FIFO_CTRL   2
#define REG_LINE_CTRL   3
#define REG_MODEM_CTRL  4
#define REG_LINE_STATUS 5

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

void serial_init(void)
{
    outb(COM1 + REG_INT_ENABLE, 0x00);  /* no interrupts, we poll            */
    outb(COM1 + REG_LINE_CTRL, 0x80);   /* DLAB on: divisor latch accessible */
    outb(COM1 + REG_DIVISOR_LO, 0x01);  /* divisor 1 => 115200 baud          */
    outb(COM1 + REG_DIVISOR_HI, 0x00);
    outb(COM1 + REG_LINE_CTRL, 0x03);   /* DLAB off, 8 bits, no parity, 1 stop */
    outb(COM1 + REG_FIFO_CTRL, 0xC7);   /* enable + clear FIFOs, 14-byte trigger */
    outb(COM1 + REG_MODEM_CTRL, 0x0B);  /* DTR + RTS + OUT2                  */
}

static void serial_wait_tx(void)
{
    while (!(inb(COM1 + REG_LINE_STATUS) & LSR_THR_EMPTY))
        ;
}

void serial_putc(char c)
{
    /* Terminals expect CRLF; the kernel only ever emits '\n'. */
    if (c == '\n') {
        serial_wait_tx();
        outb(COM1 + REG_DATA, '\r');
    }
    serial_wait_tx();
    outb(COM1 + REG_DATA, (uint8_t)c);
}

void serial_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        serial_putc(s[i]);
}

bool serial_has_input(void)
{
    return (inb(COM1 + REG_LINE_STATUS) & LSR_DATA_READY) != 0;
}

char serial_getc(void)
{
    while (!serial_has_input())
        ;
    return (char)inb(COM1 + REG_DATA);
}
