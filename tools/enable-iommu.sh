#!/bin/sh
# Turn the IOMMU on, so the wireless adapter can be handed to a virtual machine.
#
# This is the one step tools/wifi-passthrough.sh cannot do for itself: VFIO
# needs an IOMMU to isolate a device with, the IOMMU is off unless the kernel
# is told to use it, and the kernel is told at boot.  So this edits the boot
# loader's configuration and the machine has to be restarted afterwards.
#
#   sudo tools/enable-iommu.sh          add the parameters
#   sudo tools/enable-iommu.sh --undo   take them back out
#   tools/enable-iommu.sh --check       say what the state is (no root needed)
#
# This is the only thing in this repository that changes the machine it is
# built on rather than the machine being built, which is worth saying out
# loud.  It touches exactly one line of /etc/default/grub, keeps a timestamped
# backup, checks that the regenerated boot configuration really contains what
# was asked for, and puts the backup back if it does not.

set -eu

PARAMS="intel_iommu=on iommu=pt"
GRUB_DEFAULT_FILE="/etc/default/grub"
GRUB_CFG="/boot/grub/grub.cfg"

# ---------------------------------------------------------------------------
# What is true now
# ---------------------------------------------------------------------------

iommu_active() {
    [ -d /sys/kernel/iommu_groups ] &&
    [ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]
}

report() {
    echo "firmware VT-d (DMAR table) : $([ -e /sys/firmware/acpi/tables/DMAR ] &&
                                          echo present || echo "MISSING -- enable VT-d in the firmware setup")"
    echo "kernel command line        : $(cat /proc/cmdline)"

    if iommu_active; then
        echo "IOMMU groups               : $(ls /sys/kernel/iommu_groups | wc -l) -- the IOMMU is on"
    else
        echo "IOMMU groups               : none -- the IOMMU is off"
    fi

    if grep -q "intel_iommu=on" "$GRUB_DEFAULT_FILE" 2>/dev/null; then
        echo "$GRUB_DEFAULT_FILE      : already carries the parameters"
        if ! iommu_active; then
            echo
            echo "  The parameters are configured but not in force: restart to pick them up."
        fi
    else
        echo "$GRUB_DEFAULT_FILE      : does not carry the parameters"
    fi
}

if [ "${1:-}" = "--check" ]; then
    report
    exit 0
fi

if [ "$(id -u)" != "0" ]; then
    echo "this needs root: sudo $0 ${1:-}" >&2
    echo >&2
    report >&2
    exit 1
fi

if [ ! -f "$GRUB_DEFAULT_FILE" ]; then
    echo "no $GRUB_DEFAULT_FILE -- this machine does not boot with GRUB," >&2
    echo "so add '$PARAMS' to its kernel command line by hand." >&2
    exit 1
fi

if ! command -v grub-mkconfig >/dev/null 2>&1; then
    echo "grub-mkconfig is not installed; not touching the configuration" >&2
    exit 1
fi

BACKUP="$GRUB_DEFAULT_FILE.before-iommu.$(date +%Y%m%d-%H%M%S)"

# ---------------------------------------------------------------------------
# Regenerate, and check it took.  A boot loader configuration that did not get
# written is the one failure here that matters, so it is verified rather than
# assumed -- and the backup goes back if anything is wrong.
# ---------------------------------------------------------------------------

regenerate() {
    want="$1"           # a string grub.cfg must contain, or "" for must-not

    echo "regenerating $GRUB_CFG"
    if ! grub-mkconfig -o "$GRUB_CFG" 2>&1 | sed 's/^/  /'; then
        echo "grub-mkconfig failed; putting $GRUB_DEFAULT_FILE back" >&2
        cp "$BACKUP" "$GRUB_DEFAULT_FILE"
        exit 1
    fi

    if [ -n "$want" ] && ! grep -q "$want" "$GRUB_CFG"; then
        echo "the new $GRUB_CFG does not mention '$want'; putting the old" >&2
        echo "configuration back and changing nothing" >&2
        cp "$BACKUP" "$GRUB_DEFAULT_FILE"
        grub-mkconfig -o "$GRUB_CFG" >/dev/null 2>&1 || true
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Take them out again
# ---------------------------------------------------------------------------

if [ "${1:-}" = "--undo" ]; then
    if ! grep -q "intel_iommu=on" "$GRUB_DEFAULT_FILE"; then
        echo "$GRUB_DEFAULT_FILE does not carry the parameters; nothing to undo"
        exit 0
    fi

    cp "$GRUB_DEFAULT_FILE" "$BACKUP"
    echo "backup: $BACKUP"

    sed -i 's/ *intel_iommu=on//; s/ *iommu=pt//' "$GRUB_DEFAULT_FILE"

    grep '^GRUB_CMDLINE_LINUX_DEFAULT=' "$GRUB_DEFAULT_FILE" | sed 's/^/  now: /'
    regenerate ""
    echo
    echo "done -- restart for the IOMMU to go back off"
    exit 0
fi

# ---------------------------------------------------------------------------
# Put them in
# ---------------------------------------------------------------------------

if grep -q "intel_iommu=on" "$GRUB_DEFAULT_FILE"; then
    echo "$GRUB_DEFAULT_FILE already carries the parameters; leaving it alone"
    echo
    if iommu_active; then
        echo "and the IOMMU is on -- tools/wifi-passthrough.sh will work"
    else
        echo "but the IOMMU is not on yet: restart to pick them up"
    fi
    exit 0
fi

if [ ! -e /sys/firmware/acpi/tables/DMAR ]; then
    echo "warning: this machine reports no DMAR table, which means the firmware" >&2
    echo "         is not offering VT-d.  Turn it on in the firmware setup"  >&2
    echo "         (often \"VT-d\", \"Intel Virtualization Technology for"    >&2
    echo "         Directed I/O\"), or these parameters will do nothing."     >&2
    echo >&2
fi

cp "$GRUB_DEFAULT_FILE" "$BACKUP"
echo "backup: $BACKUP"

# Append inside the existing quotes of the one line that holds the ordinary
# kernel parameters.  Anchored to the start of the line so that a mention of
# the variable in a comment cannot be edited by mistake.
sed -i "s/^GRUB_CMDLINE_LINUX_DEFAULT=\"\(.*\)\"/GRUB_CMDLINE_LINUX_DEFAULT=\"\1 $PARAMS\"/" \
    "$GRUB_DEFAULT_FILE"

if ! grep -q "intel_iommu=on" "$GRUB_DEFAULT_FILE"; then
    echo "could not edit GRUB_CMDLINE_LINUX_DEFAULT; putting the backup back" >&2
    cp "$BACKUP" "$GRUB_DEFAULT_FILE"
    exit 1
fi

grep '^GRUB_CMDLINE_LINUX_DEFAULT=' "$GRUB_DEFAULT_FILE" | sed 's/^/  now: /'

regenerate "intel_iommu=on"

cat <<'EOF'

done.  The parameters are in the boot configuration but not in force: the
kernel reads them at boot, so this needs a restart.

After restarting:

    tools/enable-iommu.sh --check          should list IOMMU groups
    sudo tools/wifi-passthrough.sh         run WOS with the real adapter

To put things back:

    sudo tools/enable-iommu.sh --undo
EOF
