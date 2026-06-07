/* Processor identification, clock and temperature.  See cpu.h. */

#include "cpu.h"
#include "acpi.h"
#include "pit.h"
#include "sched.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* ------------------------------------------------------------------ *
 *  The registers this file reads
 * ------------------------------------------------------------------ */

#define MSR_PLATFORM_INFO      0x0CE   /* the ratios the part is built for  */
#define MSR_MPERF              0x0E7   /* ticks at the base clock           */
#define MSR_APERF              0x0E8   /* ticks at the clock actually run   */
#define MSR_PERF_CTL           0x199   /* ask for a clock, the old way      */
#define MSR_THERM_STATUS       0x19C   /* the digital thermal sensor        */
#define MSR_TEMPERATURE_TARGET 0x1A2   /* where that sensor counts down from */
#define MSR_TURBO_RATIO_LIMIT  0x1AD   /* fastest ratio with one core busy  */
#define MSR_PM_ENABLE          0x770   /* is hardware-managed control on?   */
#define MSR_HWP_CAPABILITIES   0x771   /* the range it will work within     */
#define MSR_HWP_REQUEST        0x774   /* ask for a clock, the new way      */

/* Defined in msr.S: a read or write that returns false instead of faulting on
 * a machine without that register. */
extern bool msr_try_read(uint32_t msr, uint64_t *out);
extern bool msr_try_write(uint32_t msr, uint64_t value);

/* CPUID leaf 6 (thermal and power management). */
#define CPUID_THERM_DTS   (1u << 0)    /* EAX: digital thermal sensor       */
#define CPUID_THERM_HWP   (1u << 7)    /* EAX: hardware-managed performance */
#define CPUID_THERM_APERF (1u << 0)    /* ECX: APERF/MPERF are implemented  */

/* IA32_PM_ENABLE bit 0: hardware-managed performance states are switched on,
 * which is what decides whether IA32_HWP_REQUEST or the older IA32_PERF_CTL is
 * the register the processor is listening to. */
#define HWP_ENABLE (1ull << 0)

/* The three performance fields of IA32_HWP_REQUEST, in ratio units: minimum,
 * maximum and desired.  Writing the same value to all three pins the clock. */
#define HWP_PERF_FIELDS 0x00FFFFFFull

/* When the temperature target register is missing, 100 degrees is the value
 * Intel's parts have used throughout the range this kernel can run on.  It is
 * a floor for the reading rather than a guess at the part: the sensor reports
 * how far below the target it is, so being wrong by a few degrees shifts the
 * number without changing what it is watching for. */
#define TEMPERATURE_TARGET_DEFAULT 100

/* The reference clock every Intel ratio is a multiple of, when CPUID does not
 * say what it is.  100 MHz on everything since Sandy Bridge. */
#define BUS_KHZ_DEFAULT 100000u

static inline void cpuid_leaf(uint32_t leaf, uint32_t sub, uint32_t out[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(sub));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ------------------------------------------------------------------ *
 *  What we found
 * ------------------------------------------------------------------ */

struct core {
    uint32_t apic_id;
    bool     present;        /* the firmware listed it                     */
    bool     online;         /* the kernel executes on it                  */
    uint32_t clock_khz;      /* last measurement, 0 if there has been none */
    uint32_t clock_source;   /* W_CLOCK_*                                  */
    uint32_t busy_ticks;
    uint32_t idle_ticks;
};

static struct core cores[W_CPU_MAX];
static int         core_count;
static int         boot_core;

static char     brand[52];
static uint32_t tsc_khz;         /* the rate the timestamp counter runs at */
static uint32_t base_khz;        /* what the part is specified at          */
static uint32_t min_khz, max_khz;
static uint32_t bus_khz = BUS_KHZ_DEFAULT;

static bool base_is_quoted;      /* base_khz came from CPUID, not a stopwatch */
static bool have_aperf;          /* APERF/MPERF can be read                */
static bool have_dts;            /* the thermal sensor can be read         */
static bool have_hwp;            /* the processor manages its own clock,
                                  * and takes requests through HWP_REQUEST */
static bool can_set_clock;       /* there is a register here to write      */
static uint32_t pinned_khz;      /* what the clock was last held at, 0 for
                                  * whatever the hardware decides           */
static int  temp_target = TEMPERATURE_TARGET_DEFAULT;

static bool ready;               /* cpu_init() has run                     */

/* ------------------------------------------------------------------ *
 *  Enumeration
 * ------------------------------------------------------------------ */

/* The fixed header every ACPI table starts with, repeated here so this file
 * can step over one without depending on acpi.c's private structures. */
#define ACPI_HEADER_SIZE 36

/* MADT entry types.  Anything else in the table is a controller, not a core. */
#define MADT_LOCAL_APIC   0
#define MADT_LOCAL_X2APIC 9

/* The two flags a processor entry can carry: usable now, or startable later.
 * Both mean the core is really there, which is all this counts. */
#define MADT_ENABLED        (1u << 0)
#define MADT_ONLINE_CAPABLE (1u << 1)

static void add_core(uint32_t apic_id)
{
    if (core_count >= W_CPU_MAX)
        return;

    cores[core_count].apic_id = apic_id;
    cores[core_count].present = true;
    core_count++;
}

/* Walk the ACPI processor list.  Returns how many cores it described, which is
 * zero on a machine whose firmware provided no MADT. */
static int read_madt(void)
{
    uint32_t length = 0;
    uint8_t *madt = acpi_table("APIC", &length);
    if (!madt)
        return 0;

    /* 8 bytes past the header: the local controller address and its flags. */
    uint32_t at  = ACPI_HEADER_SIZE + 8;

    while (at + 2 <= length) {
        uint8_t type = madt[at];
        uint8_t len  = madt[at + 1];

        if (len < 2 || at + len > length)
            break;                       /* a malformed entry ends the walk */

        if (type == MADT_LOCAL_APIC && len >= 8) {
            uint32_t flags;
            memcpy(&flags, madt + at + 4, 4);
            if (flags & (MADT_ENABLED | MADT_ONLINE_CAPABLE))
                add_core(madt[at + 3]);
        } else if (type == MADT_LOCAL_X2APIC && len >= 16) {
            uint32_t id, flags;
            memcpy(&id, madt + at + 4, 4);
            memcpy(&flags, madt + at + 8, 4);
            if (flags & (MADT_ENABLED | MADT_ONLINE_CAPABLE))
                add_core(id);
        }

        at += len;
    }

    kfree(madt);
    return core_count;
}

/* This processor's identifier, to find it in the list the firmware gave. */
static uint32_t local_apic_id(void)
{
    uint32_t r[4];

    cpuid_leaf(0, 0, r);
    if (r[0] >= 0x0B) {
        cpuid_leaf(0x0B, 0, r);
        return r[3];                     /* EDX: the full x2APIC id */
    }

    cpuid_leaf(1, 0, r);
    return r[1] >> 24;                   /* EBX 31:24: the 8-bit one */
}

static void read_brand(void)
{
    uint32_t r[4];

    cpuid_leaf(0x80000000, 0, r);
    if (r[0] < 0x80000004)
        return;

    for (uint32_t i = 0; i < 3; i++) {
        cpuid_leaf(0x80000002 + i, 0, r);
        memcpy(brand + i * 16, r, 16);
    }
    brand[48] = '\0';

    /* The string is padded with spaces on the left on some parts and on the
     * right on others; neither belongs in what gets printed. */
    char *start = brand;
    while (*start == ' ')
        start++;
    if (start != brand)
        memmove(brand, start, strlen(start) + 1);

    for (int i = (int)strlen(brand) - 1; i >= 0 && brand[i] == ' '; i--)
        brand[i] = '\0';
}

/* ------------------------------------------------------------------ *
 *  How fast it goes
 * ------------------------------------------------------------------ */

/* How long to wait for the tick count to move before concluding that it never
 * will.  The bound is on spins that made no progress rather than on the
 * measurement as a whole: a budget for the whole thing would have to exceed
 * the number of times the fastest imaginable machine can go round this loop in
 * a fifth of a second, and that is a number which grows with every new CPU
 * until one day it declares a working timer frozen.
 *
 * It has to be bounded at all because a firmware that routed the timer through
 * the IOAPIC rather than the legacy PIC leaves the count still, and an
 * unbounded wait here would hang the boot outright. */
#define TICK_WAIT_SPINS 200000000u

/* Wait for the tick count to change.  False if it never did. */
static bool wait_for_tick(void)
{
    uint32_t t = pit_ticks();

    for (uint32_t spins = 0; spins <= TICK_WAIT_SPINS; spins++)
        if (pit_ticks() != t)
            return true;

    return false;
}

/* Count timestamp counter cycles across a whole number of timer ticks.
 *
 * Both ends of the measurement are taken immediately after the tick count
 * changed, so the interval really is the number of ticks asked for and not
 * some fraction more: the error is the interrupt latency at each end, tens of
 * microseconds against a window of a fifth of a second.
 *
 * This is the last resort.  A processor that can be asked outright is, first.
 * Returns 0 if the timer is not running, since there is then nothing to
 * measure against. */
static uint32_t measure_tsc_khz(uint32_t ticks)
{
    if (!wait_for_tick())
        return 0;

    uint64_t from = rdtsc();

    for (uint32_t i = 0; i < ticks; i++)
        if (!wait_for_tick())
            return 0;

    uint64_t cycles = rdtsc() - from;
    uint64_t ms     = (uint64_t)ticks * (1000u / PIT_HZ);

    return (uint32_t)(cycles / ms);      /* cycles per millisecond is kHz */
}

/* The clock the timestamp counter and the performance counters are scaled to.
 *
 * CPUID leaf 0x15 gives it exactly, as a ratio against the core crystal;
 * leaf 0x16 gives the base frequency in whole megahertz, which is the same
 * number rounded.  Only when neither is there does this fall back to timing
 * the counter against the interval timer. */
static void find_tsc_rate(void)
{
    uint32_t r[4], max_leaf;

    cpuid_leaf(0, 0, r);
    max_leaf = r[0];

    if (max_leaf >= 0x15) {
        cpuid_leaf(0x15, 0, r);
        if (r[0] && r[1] && r[2])
            tsc_khz = (uint32_t)(((uint64_t)r[2] / 1000) * r[1] / r[0]);
    }

    if (max_leaf >= 0x16) {
        cpuid_leaf(0x16, 0, r);
        if (r[0]) { base_khz = r[0] * 1000; base_is_quoted = true; }
        if (r[1]) max_khz  = r[1] * 1000;
        if (r[2]) bus_khz  = r[2] * 1000;
        if (!tsc_khz)
            tsc_khz = base_khz;
    }

    if (!tsc_khz)
        tsc_khz = measure_tsc_khz(20);   /* a fifth of a second */
    if (!base_khz)
        base_khz = tsc_khz;
    if (!max_khz)
        max_khz = base_khz;
}

/* The slowest and fastest the part says it will run.  Both come from registers
 * a hypervisor is unlikely to implement, so both are allowed to stay unknown --
 * every caller treats a zero as "the machine did not say". */
static void find_ratio_limits(void)
{
    uint64_t value;

    if (msr_try_read(MSR_PLATFORM_INFO, &value) && value) {
        uint32_t base_ratio = (value >> 8) & 0xFF;    /* max without turbo */
        uint32_t min_ratio  = (value >> 40) & 0xFF;   /* most efficient    */

        if (min_ratio)
            min_khz = min_ratio * bus_khz;
        if (base_ratio && !base_khz)
            base_khz = base_ratio * bus_khz;
    }

    if (msr_try_read(MSR_TURBO_RATIO_LIMIT, &value)) {
        uint32_t turbo = value & 0xFF;                /* one core busy */
        if (turbo && turbo * bus_khz > max_khz)
            max_khz = turbo * bus_khz;
    }

    /* A processor managing its own clock states the range it will work in
     * directly, and that is the range a request has to fall inside. */
    if (have_hwp && msr_try_read(MSR_HWP_CAPABILITIES, &value) && value) {
        uint32_t highest = value & 0xFF;
        uint32_t lowest  = (value >> 24) & 0xFF;

        if (lowest)
            min_khz = lowest * bus_khz;
        if (highest && highest * bus_khz > max_khz)
            max_khz = highest * bus_khz;
    }
}

/* ------------------------------------------------------------------ *
 *  Asking for a different clock
 * ------------------------------------------------------------------ */

/* Which register this machine takes clock requests through, if any.
 *
 * Two mechanisms, and only one of them is listened to at a time.  A processor
 * with hardware-managed performance states switched on decides its own clock
 * and takes a range through IA32_HWP_REQUEST; the older arrangement has the
 * operating system name a ratio in IA32_PERF_CTL.  Writing to the one that is
 * not in force is not an error and not a change either -- the write lands and
 * nothing happens -- so which is which has to be established here rather than
 * discovered from the clock afterwards. */
/* Can this register be read, and then written back exactly as it was?
 *
 * Being readable is not enough.  A hypervisor answers some of these registers
 * and faults on writing them, so the only way to know a request could be made
 * is to make one -- and putting back the value that is already there is a
 * request for what the machine is doing anyway, which changes nothing whether
 * it works or not. */
static bool writable(uint32_t msr)
{
    uint64_t value;

    return msr_try_read(msr, &value) && msr_try_write(msr, value);
}

static void find_clock_control(void)
{
    uint64_t value;

    if (have_hwp && msr_try_read(MSR_PM_ENABLE, &value) && (value & HWP_ENABLE)) {
        can_set_clock = writable(MSR_HWP_REQUEST);
        if (can_set_clock)
            return;
    }

    have_hwp      = false;
    can_set_clock = writable(MSR_PERF_CTL);
}

/* Turn a clock into the ratio the hardware actually deals in: every step is a
 * multiple of the bus reference, so 1750 MHz on a 100 MHz bus is a request for
 * either 1.7 or 1.8 GHz and nothing in between. */
static uint32_t ratio_for(uint32_t khz)
{
    uint32_t ratio = (khz + bus_khz / 2) / bus_khz;

    if (ratio < 1)
        ratio = 1;
    if (ratio > 255)
        ratio = 255;
    return ratio;
}

/* Check that the request is still there after being made.
 *
 * A register that accepts a write and forgets it is exactly what a hypervisor
 * provides: KVM answers IA32_PERF_CTL rather than faulting on it, so the write
 * lands, nothing happens, and without this the machine would report a clock it
 * had been told to hold and was never going to.  Only the field that was
 * written is compared; the rest of the register belongs to the hardware. */
static bool request_took(uint32_t msr, uint32_t shift, uint32_t ratio)
{
    uint64_t value;

    if (!msr_try_read(msr, &value))
        return false;

    if (((value >> shift) & 0xFF) == ratio)
        return true;

    /* Nothing here is listening, and nothing will be later either. */
    can_set_clock = false;
    return false;
}

int cpu_set_khz(uint32_t khz)
{
    uint64_t value;

    if (!can_set_clock || !bus_khz)
        return -W_ENODEV;

    if (min_khz && khz < min_khz)
        khz = min_khz;
    if (max_khz && khz > max_khz)
        khz = max_khz;

    uint32_t ratio = ratio_for(khz);

    if (have_hwp) {
        if (!msr_try_read(MSR_HWP_REQUEST, &value))
            return -W_EIO;

        /* Floor, ceiling and target all at one value: a range of one is how
         * this interface is told to hold still. */
        value &= ~HWP_PERF_FIELDS;
        value |= ratio | (ratio << 8) | (ratio << 16);

        if (!msr_try_write(MSR_HWP_REQUEST, value)) {
            can_set_clock = false;
            return -W_ENODEV;
        }
        if (!request_took(MSR_HWP_REQUEST, 16, ratio))
            return -W_ENODEV;
    } else {
        if (!msr_try_read(MSR_PERF_CTL, &value))
            return -W_EIO;

        value &= ~0xFF00ull;
        value |= (uint64_t)ratio << 8;

        if (!msr_try_write(MSR_PERF_CTL, value)) {
            can_set_clock = false;
            return -W_ENODEV;
        }
        if (!request_took(MSR_PERF_CTL, 8, ratio))
            return -W_ENODEV;
    }

    pinned_khz = ratio * bus_khz;
    return (int)pinned_khz;
}

int cpu_set_automatic(void)
{
    uint64_t value;

    if (!can_set_clock)
        return -W_ENODEV;

    if (have_hwp) {
        if (!msr_try_read(MSR_HWP_CAPABILITIES, &value))
            return -W_EIO;

        uint32_t highest = value & 0xFF;
        uint32_t lowest  = (value >> 24) & 0xFF;

        if (!msr_try_read(MSR_HWP_REQUEST, &value))
            return -W_EIO;

        /* The whole range back, and a desired performance of zero, which is
         * this interface's way of saying "you decide". */
        value &= ~HWP_PERF_FIELDS;
        value |= lowest | (highest << 8);

        if (!msr_try_write(MSR_HWP_REQUEST, value)) {
            can_set_clock = false;
            return -W_ENODEV;
        }
    } else {
        /* Without hardware management there is no "decide for yourself": the
         * ratio in the register is the whole policy, so the honest equivalent
         * is to ask for the fastest the part is rated at. */
        if (!msr_try_read(MSR_PERF_CTL, &value))
            return -W_EIO;

        value &= ~0xFF00ull;
        value |= (uint64_t)ratio_for(max_khz ? max_khz : base_khz) << 8;

        if (!msr_try_write(MSR_PERF_CTL, value)) {
            can_set_clock = false;
            return -W_ENODEV;
        }
    }

    pinned_khz = 0;
    return 0;
}

/* Both performance counters, read together so the ratio between them covers
 * the same span of time. */
static bool read_perf_counters(uint64_t *aperf, uint64_t *mperf)
{
    return msr_try_read(MSR_APERF, aperf) && msr_try_read(MSR_MPERF, mperf) &&
           *mperf != 0;
}

/* ------------------------------------------------------------------ *
 *  Sampling, once a tick
 * ------------------------------------------------------------------ */

static uint64_t last_aperf, last_mperf;

/* Work out what the core has been running at since the last tick.
 *
 * MPERF counts at the base clock and APERF at whatever the core is actually
 * being clocked at, so their ratio scales the one known frequency into the
 * real one.  Nothing else on a modern part will say: the timestamp counter
 * deliberately runs at a fixed rate however fast the core is going, which is
 * what makes it a clock and useless as a speedometer. */
/* The best a machine with nothing to measure can be asked: the clock it is
 * specified at, which is what it runs at when it is neither throttling itself
 * nor in turbo.  Saying where that came from is the point of clock_source --
 * a program showing this can say "base" rather than implying it watched. */
static void quote_clock(struct core *c)
{
    if (base_is_quoted) {
        c->clock_khz    = base_khz;
        c->clock_source = W_CLOCK_CPUID;
    } else {
        c->clock_khz    = tsc_khz;
        c->clock_source = tsc_khz ? W_CLOCK_TSC : W_CLOCK_NONE;
    }
}

static void sample_clock(struct core *c)
{
    if (!have_aperf) {
        quote_clock(c);
        return;
    }

    uint64_t aperf, mperf;
    if (!read_perf_counters(&aperf, &mperf))
        return;

    uint64_t da = aperf - last_aperf;      /* wraps correctly either way */
    uint64_t dm = mperf - last_mperf;

    last_aperf = aperf;
    last_mperf = mperf;

    if (!dm || !da)
        return;                            /* keep the previous reading */

    /* Scale down before multiplying: a tick's worth of counts is small, but a
     * first sample taken against a zero baseline is a whole boot's worth and
     * would overflow the product. */
    while (da > (uint64_t)-1 / tsc_khz) {
        da /= 2;
        dm /= 2;
    }
    if (!dm)
        return;

    c->clock_khz    = (uint32_t)(da * tsc_khz / dm);
    c->clock_source = W_CLOCK_APERF;
}

void cpu_tick(void)
{
    if (!ready)
        return;

    struct core *c = &cores[boot_core];

    if (sched_current_is_idle())
        c->idle_ticks++;
    else
        c->busy_ticks++;

    sample_clock(c);
}

/* ------------------------------------------------------------------ *
 *  Temperature
 * ------------------------------------------------------------------ */

/* The sensor does not report a temperature but a distance: how many degrees
 * below the point at which this part throttles itself the core currently is.
 * A reading of 30 on a machine that throttles at 100 is a core at 70. */
static int read_temperature(void)
{
    uint64_t status;

    if (!have_dts || !msr_try_read(MSR_THERM_STATUS, &status))
        return W_TEMP_UNKNOWN;

    if (!(status & (1u << 31)))            /* the reading is not valid yet */
        return W_TEMP_UNKNOWN;

    int below = (int)((status >> 16) & 0x7F);
    return temp_target - below;
}

static void find_thermal_target(void)
{
    uint64_t value;

    if (msr_try_read(MSR_TEMPERATURE_TARGET, &value)) {
        int target = (int)((value >> 16) & 0xFF);
        if (target > 0)
            temp_target = target;
    }
}

/* ------------------------------------------------------------------ *
 *  What the rest of the kernel asks for
 * ------------------------------------------------------------------ */

void cpu_info(wcpuinfo_t *out)
{
    memset(out, 0, sizeof(*out));

    out->count      = core_count;
    out->online     = ready ? 1 : 0;
    out->tick_hz    = PIT_HZ;
    out->base_khz   = base_khz;
    out->min_khz    = min_khz;
    out->max_khz    = max_khz;
    out->settable   = can_set_clock ? 1 : 0;
    out->step_khz   = can_set_clock ? bus_khz : 0;
    out->pinned_khz = pinned_khz;
    strlcpy(out->brand, brand, sizeof(out->brand));
}

int cpu_list(wcpu_t *out, int max)
{
    if (max > core_count)
        max = core_count;

    for (int i = 0; i < max; i++) {
        const struct core *c = &cores[i];

        memset(&out[i], 0, sizeof(out[i]));
        out[i].id           = i;
        out[i].apic_id      = c->apic_id;
        out[i].online       = c->online ? 1 : 0;
        out[i].clock_khz    = c->clock_khz;
        out[i].clock_source = c->clock_source;
        out[i].busy_ticks   = c->busy_ticks;
        out[i].idle_ticks   = c->idle_ticks;

        /* A temperature has to be read by the core it belongs to, and only one
         * core is running any of this. */
        out[i].temp_c     = c->online ? read_temperature() : W_TEMP_UNKNOWN;
        out[i].temp_max_c = c->online && have_dts ? temp_target : W_TEMP_UNKNOWN;
    }

    return max;
}

uint32_t cpu_tsc_khz(void)
{
    return tsc_khz;
}

void cpu_print_report(void)
{
    if (brand[0])
        kprintf("cpu    : %s\n", brand);

    kprintf("cpu    : %d core%s, %s base", core_count,
            core_count == 1 ? "" : "s", fmt_khz(base_khz));
    if (min_khz)
        kprintf(", %s idle", fmt_khz(min_khz));
    if (max_khz && max_khz != base_khz)
        kprintf(", %s flat out", fmt_khz(max_khz));
    kprintf(" (%s counter)\n", fmt_khz(tsc_khz));

    kprintf("cpu    : clock %s, temperature %s\n",
            have_aperf ? "measured every tick"
                       : "not measurable here -- no APERF/MPERF counters",
            have_dts ? "from the on-die sensor"
                     : "not readable here -- no thermal sensor");

    kprintf("cpu    : clock %s\n",
            !can_set_clock ? "cannot be set on this machine"
                           : have_hwp ? "settable, through hardware-managed "
                                        "performance states"
                                      : "settable, through the performance "
                                        "control register");

    if (core_count > 1)
        kprintf("cpu    : running on core %d; the other %d are listed but "
                "never started\n", boot_core, core_count - 1);
}

void cpu_init(void)
{
    read_brand();

    if (read_madt() == 0)
        add_core(local_apic_id());       /* no processor list: just this one */

    uint32_t me = local_apic_id();
    for (int i = 0; i < core_count; i++)
        if (cores[i].apic_id == me)
            boot_core = i;
    cores[boot_core].online = true;

    uint32_t r[4], max_leaf;
    cpuid_leaf(0, 0, r);
    max_leaf = r[0];

    if (max_leaf >= 6) {
        cpuid_leaf(6, 0, r);
        have_dts   = (r[0] & CPUID_THERM_DTS) != 0;
        have_hwp   = (r[0] & CPUID_THERM_HWP) != 0;
        have_aperf = (r[2] & CPUID_THERM_APERF) != 0;
    }

    find_tsc_rate();
    find_clock_control();
    find_ratio_limits();
    if (have_dts)
        find_thermal_target();

    /* A counter pair that cannot actually be read -- the usual answer inside a
     * hypervisor -- is no better than one the part never had. */
    if (have_aperf && !read_perf_counters(&last_aperf, &last_mperf))
        have_aperf = false;

    /* Something to report before the first tick has been sampled. */
    quote_clock(&cores[boot_core]);

    ready = true;
}
