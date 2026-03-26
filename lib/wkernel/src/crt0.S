/* C runtime startup for WOS applications.
 *
 * The kernel enters a new process with the stack laid out as
 *
 *     [argc][argv[0]][argv[1]]...[NULL]  <- esp
 *
 * which is exactly what main(int, char **) wants, so all this has to do is
 * turn it into a call and then hand main's return value to wexit.  Placing
 * this in .text.start makes the linker script put _start at the very front of
 * the image, where the ELF entry point points.
 */

.section .text.start, "ax"
.global _start
.type _start, @function
_start:
    /* A zeroed frame pointer terminates any stack walk cleanly. */
    xorl  %ebp, %ebp

    movl  (%esp), %eax          /* argc */
    leal  4(%esp), %edx         /* argv */

    pushl %edx
    pushl %eax
    call  main
    addl  $8, %esp

    /* main returned, so exit with its value. wexit never comes back. */
    pushl %eax
    call  wexit

    /* Unreachable: stop the CPU rather than run into whatever follows. */
1:  hlt
    jmp 1b
.size _start, . - _start
