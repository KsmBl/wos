/* Model-specific register access that survives a machine that has not got the
 * register being asked for.
 *
 * RDMSR and WRMSR raise a general protection fault for any address the CPU does
 * not implement, and there is no way to ask in advance whether a given one is
 * there.  CPUID feature bits cover some of them; the rest -- the temperature
 * target, the platform's ratio limits -- are documented per model and simply
 * absent elsewhere.  A hypervisor makes it worse: KVM faults on every MSR it
 * has not been told to emulate, so registers that exist on the metal are gone
 * inside a guest running on the same chip.
 *
 * So the fault is made survivable instead.  Each access is a single
 * instruction at a known, exported address; the #GP handler in traps.c
 * recognises that address and resumes at the fixup label just after it, which
 * returns failure.  The caller sees a 0 return and no fault.
 */

    .text

/* bool msr_try_read(uint32_t msr, uint64_t *out)
 *
 * edi = MSR number, rsi = where to put the value.  Returns 1 on success; on a
 * fault, returns 0 and stores 0. */
    .globl msr_try_read
    .type msr_try_read, @function
msr_try_read:
    movl    %edi, %ecx
    xorl    %eax, %eax
    xorl    %edx, %edx

    .globl msr_read_insn
msr_read_insn:
    rdmsr                       /* faults here, or not at all */

    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rax, (%rsi)
    movl    $1, %eax
    ret

    .globl msr_read_fixup
msr_read_fixup:
    movq    $0, (%rsi)
    xorl    %eax, %eax
    ret
    .size msr_try_read, . - msr_try_read

/* bool msr_try_write(uint32_t msr, uint64_t value)
 *
 * edi = MSR number, rsi = value.  Returns 1 if the write went through. */
    .globl msr_try_write
    .type msr_try_write, @function
msr_try_write:
    movl    %edi, %ecx
    movq    %rsi, %rax
    movq    %rsi, %rdx
    shrq    $32, %rdx

    .globl msr_write_insn
msr_write_insn:
    wrmsr                       /* faults here, or not at all */

    movl    $1, %eax
    ret

    .globl msr_write_fixup
msr_write_fixup:
    xorl    %eax, %eax
    ret
    .size msr_try_write, . - msr_try_write

    .section .note.GNU-stack,"",@progbits
