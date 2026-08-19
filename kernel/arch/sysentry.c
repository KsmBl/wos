/* Turning on the SYSCALL instruction.
 *
 * The stub in sysentry.S is only reachable once the processor has been told
 * where it lives and what to load when it gets there, which is four MSRs and a
 * bit in a fifth.  All of them are per-processor, so every core runs this for
 * itself; the entry point and the segment layout it implies are the same
 * everywhere, only the stack block differs.
 *
 * int 0x80 is left installed and working.  Programs already on the disk were
 * built against it, the kernel path behind the two is identical, and keeping
 * it costs one IDT entry.
 */

#include "sysentry.h"
#include "gdt.h"
#include "smp.h"
#include "kprintf.h"
#include "string.h"

#define IA32_EFER           0xC0000080u
#define IA32_STAR           0xC0000081u
#define IA32_LSTAR          0xC0000082u
#define IA32_FMASK          0xC0000084u
#define IA32_KERNEL_GS_BASE 0xC0000102u

#define EFER_SCE (1u << 0)

/* RFLAGS bits cleared on entry.
 *
 * IF because the int 0x80 gate is an interrupt gate and clears it, and the
 * kernel behind them is one body of code that should not have to ask how it
 * was entered.  DF because the C code below assumes forward string operations,
 * as every SysV compiler does.  TF and NT because a process should not be able
 * to hand the kernel a single-step or a nested-task flag. */
#define SYSCALL_FLAG_MASK 0x00047700u

extern void syscall_entry(void);

/* One per processor, reached through GS inside the stub.  The offsets are part
 * of the contract with sysentry.S: kernel_rsp at 0, user_rsp at 8. */
static sysentry_cpu_t blocks[MAX_CPUS];

_Static_assert(__builtin_offsetof(sysentry_cpu_t, kernel_rsp) == 0,
               "sysentry.S loads the kernel stack from gs:0");
_Static_assert(__builtin_offsetof(sysentry_cpu_t, user_rsp) == 8,
               "sysentry.S stashes the user stack at gs:8");

/* SYSCALL takes CS from STAR[47:32] and SS from that plus eight.  SYSRET takes
 * SS from STAR[63:48] plus eight and CS from that plus sixteen, forcing ring 3
 * on both.  That is not a convention this kernel gets to choose -- it is what
 * fixes the order of the descriptors in the table, and a table in any other
 * order would return to user space in the wrong segment. */
_Static_assert(SEL_KDATA == SEL_KCODE + 8,
               "SYSCALL derives the kernel stack segment from the code one");
_Static_assert((SEL_UDATA & ~3) == (SEL_KDATA & ~3) + 8,
               "SYSRET derives the user stack segment from STAR[63:48] + 8");
_Static_assert((SEL_UCODE & ~3) == (SEL_KDATA & ~3) + 16,
               "SYSRET derives the user code segment from STAR[63:48] + 16");

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
                         "c"(msr)
                     : "memory");
}

static bool supported(void)
{
    uint32_t eax = 0x80000001u, ebx = 0, ecx = 0, edx = 0;

    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));

    return (edx & (1u << 11)) != 0;      /* SYSCALL/SYSRET */
}

void sysentry_set_kernel_stack(uint64_t rsp0)
{
    blocks[smp_cpu_index()].kernel_rsp = rsp0;
}


bool sysentry_init_cpu(void)
{
    if (!supported())
        return false;

    int me = smp_cpu_index();

    /* The shadow GS base, not GS itself: SWAPGS in the stub is what brings it
     * into use, and the kernel runs on a GS of zero the rest of the time. */
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&blocks[me]);

    wrmsr(IA32_STAR, ((uint64_t)SEL_KCODE << 32) |
                     ((uint64_t)(SEL_KDATA | 3) << 48));
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(IA32_FMASK, SYSCALL_FLAG_MASK);

    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_SCE);
    return true;
}
