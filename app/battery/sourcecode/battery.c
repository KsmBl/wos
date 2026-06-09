/* battery -- say whether this machine has a battery, and what it is.
 *
 *   battery        what the firmware says about the pack
 *   battery -s     one line, for a prompt or a script
 *
 * The charge is the one thing not here, and the reason is worth stating rather
 * than hiding behind a dash: a laptop reports its charge through an ACPI
 * method that reads the embedded controller, and calling one means
 * interpreting AML bytecode.  WOS has no interpreter.  Everything static --
 * that there is a battery, who made it, what it holds when new -- comes out of
 * the firmware's tables without running anything, and that is what this shows.
 */

#include <wkernel.h>

static const char *chemistry_name(unsigned int chemistry)
{
    switch (chemistry) {
    case W_BATTERY_CHEM_LEAD:    return "lead acid";
    case W_BATTERY_CHEM_NICD:    return "nickel cadmium";
    case W_BATTERY_CHEM_NIMH:    return "nickel metal hydride";
    case W_BATTERY_CHEM_LION:    return "lithium ion";
    case W_BATTERY_CHEM_ZINCAIR: return "zinc air";
    case W_BATTERY_CHEM_LIPOLY:  return "lithium polymer";
    default:                     return NULL;
    }
}

static const char *state_name(unsigned int state)
{
    switch (state) {
    case W_BATTERY_CHARGING:    return "charging";
    case W_BATTERY_DISCHARGING: return "discharging";
    case W_BATTERY_FULL:        return "full";
    default:                    return "unknown";
    }
}

/* Milliwatt-hours as watt-hours with one decimal, which is how a battery is
 * labelled.  No floating point anywhere in WOS, so it is done by hand. */
static void print_wh(unsigned int mwh)
{
    wprintf("%u.%u Wh", mwh / 1000, (mwh % 1000) / 100);
}

static void print_volts(unsigned int mv)
{
    wprintf("%u.%u V", mv / 1000, (mv % 1000) / 100);
}

int main(int argc, char **argv)
{
    wbattery_t b;
    int short_form = (argc > 1 && strcmp(argv[1], "-s") == 0);

    int r = wbattery(&b);
    if (r < 0) {
        wfprintf(W_STDERR, "battery: %s\n", wstrerror(-r));
        return 1;
    }

    if (!b.present) {
        wprintf(short_form ? "none\n"
                           : "No battery: this machine runs on mains.\n");
        return 0;
    }

    if (short_form) {
        if (b.charge_percent >= 0)
            wprintf("%d%% %s\n", b.charge_percent, state_name(b.state));
        else
            wprintf("present, charge unknown\n");
        return 0;
    }

    wprintf("battery  : present\n");

    if (b.maker[0] || b.name[0])
        wprintf("pack     : %s%s%s\n", b.maker,
                (b.maker[0] && b.name[0]) ? " " : "", b.name);

    const char *chemistry = chemistry_name(b.chemistry);
    if (chemistry)
        wprintf("chemistry: %s\n", chemistry);

    if (b.design_mwh || b.design_mv) {
        wprintf("when new : ");
        if (b.design_mwh)
            print_wh(b.design_mwh);
        if (b.design_mwh && b.design_mv)
            wprintf(" at ");
        if (b.design_mv)
            print_volts(b.design_mv);
        wprintf("\n");
    }

    if (b.location[0])
        wprintf("fitted   : %s\n", b.location);

    if (b.charge_percent >= 0) {
        wprintf("charge   : %d%%, %s\n", b.charge_percent, state_name(b.state));
    } else {
        wprintf("charge   : not readable\n");
        wprintf("\n");
        wprintf("The charge is reported by an ACPI method that reads the\n");
        wprintf("embedded controller, and calling one means interpreting AML\n");
        wprintf("bytecode. WOS has no interpreter, so everything above comes\n");
        wprintf("from the firmware's tables, read without running anything,\n");
        wprintf("and the one figure that changes minute to minute is missing\n");
        wprintf("rather than guessed at.\n");
    }

    if (b.ac_online == 1)
        wprintf("mains    : connected\n");
    else if (b.ac_online == 0)
        wprintf("mains    : not connected\n");

    return 0;
}
