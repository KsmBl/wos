/* <setjmp.h> -- leaving a deep call chain in one step.
 *
 * A compiler is the reason this is here.  TCC reports an error from wherever
 * it happens to be -- ten frames inside an expression parser -- and gets back
 * to its top level with a longjmp, because unwinding by hand through every
 * function that could fail is the thing nobody manages to keep correct.
 *
 * What is saved is what the x86-64 calling convention says the callee must
 * preserve: rbx, rbp, r12-r15, the stack pointer, and where to go back to.
 * Everything else is caller-saved and belongs to the frame being abandoned.
 */
#ifndef WLIBC_SETJMP_H
#define WLIBC_SETJMP_H

/* rbx, rbp, r12, r13, r14, r15, rsp, rip. */
typedef unsigned long jmp_buf[8];

/* Returns 0 when it is called, and the value longjmp() was given when it
 * returns for the second time.
 *
 * Declared with __returns_twice__ so the compiler does not assume the code
 * after it runs once: without that, a variable kept in a register across the
 * call can hold what it held on the way down rather than what it holds now.
 * Local variables that matter across a longjmp should still be `volatile`,
 * which is the standard's rule and not this implementation's. */
int  setjmp(jmp_buf env) __attribute__((__returns_twice__));
void longjmp(jmp_buf env, int value) __attribute__((__noreturn__));

#endif /* WLIBC_SETJMP_H */
