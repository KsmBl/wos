/* clear -- clear the screen.
 *
 * Just the escape sequences: erase the display and home the cursor.  The
 * console understands them, and so does a terminal on the serial port.
 */

#include <wkernel.h>

int main(int argc, char **argv)
{
    wcls();
    return 0;
}
