/* battery -- say whether this machine has a battery, and what it is.
 *
 *   battery        what the firmware says about the pack
 *   battery -s     one line, for a prompt or a script
 *
 * The charge used to be the one thing not here, because reading it means
 * running `_BST` -- an ACPI method that talks to the embedded controller --
 * and there was no interpreter to run it with.  There is one now, so the
 * figure is shown when the firmware will give it.
 *
 * It can still be missing, and the two ways it can be missing are different
 * things.  A machine with no battery has nothing to report.  A machine with a
 * battery whose `_BST` did not run, or which has no `_BST` at all, is a laptop
 * whose firmware this kernel could not follow -- and that says so, because a
 * dash where a number should be is a question, not an answer.
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
        wprintf("The charge comes from _BST, an ACPI method that reads the\n");
        wprintf("embedded controller. The kernel has an interpreter for those\n");
        wprintf("now, so this means one of two things: the firmware declares\n");
        wprintf("no _BST for this pack, or the method used something the\n");
        wprintf("interpreter does not implement and stopped rather than\n");
        wprintf("returning a number nobody measured. The boot log says which.\n");
    }

    if (b.ac_online == 1)
        wprintf("mains    : connected\n");
    else if (b.ac_online == 0)
        wprintf("mains    : not connected\n");

    return 0;
}
