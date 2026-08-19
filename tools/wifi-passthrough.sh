#!/bin/sh
# Boot WOS in QEMU with this machine's real wireless adapter handed to it.
#
# There is no emulated wireless device in QEMU -- of any kind, from any vendor
# -- so the only way to run the iwlwifi driver against something that answers
# like the hardware is to give it the hardware.  VFIO does that: the adapter is
# taken away from Linux, bound to a stub driver that does nothing but hold it,
# and mapped into the virtual machine, which then sees the real registers.
#
#   sudo tools/wifi-passthrough.sh          run it
#   sudo tools/wifi-passthrough.sh --return give the adapter back to Linux
#
# What this needs, and does not do for you:
#
#   1. An IOMMU.  `sudo tools/enable-iommu.sh` puts the kernel parameters in
#      place; the machine then has to be restarted, because the kernel reads
#      them at boot.  Without an IOMMU /sys/kernel/iommu_groups is empty and
#      VFIO has nothing to isolate the device with; QEMU does not support
#      VFIO's no-IOMMU mode, so there is no way around the restart.
#
#   2. Root, for unbinding a driver and loading vfio-pci.
#
# While the adapter is bound to vfio-pci, **Linux has no wireless**.  Use a
# cable or a USB adapter, or run this from a console you are not reaching over
# wifi.  --return puts it back without a reboot.
#
# A warning worth reading before you spend an evening on this: the 9560 in this
# machine is a CNVi part, meaning the radio lives in the chipset and only the
# controller half sits on the PCI bus.  Those are known to pass through badly
# -- they have no function-level reset, so a guest that touches them and dies
# can leave the adapter wedged until the machine is power-cycled.  It may
# simply not work.  That is worth knowing in advance rather than concluding at
# two in the morning.

set -eu

# The adapter, by its address on the bus.  `lspci -nnk | grep -i network` says
# what this machine's is; on the laptop WOS was written on it is the Cannon
# Point-LP CNVi at 00:14.3, alongside the other chipset functions rather than
# on a slot of its own.
SLOT="0000:00:14.3"

here() { cd "$(dirname "$0")/.." && pwd; }
ROOT="$(here)"

if [ "$(id -u)" != "0" ]; then
    echo "this needs root: sudo $0 $*" >&2
    exit 1
fi

# ---- give it back -----------------------------------------------------------

if [ "${1:-}" = "--return" ]; then
    # Clear the override FIRST.  It is what pins the device to vfio-pci, and
    # while it is set, asking the bus to probe the device again simply binds
    # it straight back to vfio -- which looks exactly like the release having
    # failed for no reason.
    echo "" > "/sys/bus/pci/devices/$SLOT/driver_override" 2>/dev/null || true

    if [ -e "/sys/bus/pci/devices/$SLOT/driver" ]; then
        echo "$SLOT" > "/sys/bus/pci/devices/$SLOT/driver/unbind" 2>/dev/null || true
    fi
    echo "$SLOT" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    modprobe iwlwifi 2>/dev/null || true
    sleep 1
    echo "adapter returned to: $(basename "$(readlink -f "/sys/bus/pci/devices/$SLOT/driver" 2>/dev/null)" 2>/dev/null || echo "nothing")"
    exit 0
fi

# ---- checks -----------------------------------------------------------------

if [ ! -e "/sys/bus/pci/devices/$SLOT" ]; then
    echo "no device at $SLOT -- check lspci and edit SLOT in this script" >&2
    exit 1
fi

if [ ! -d /sys/kernel/iommu_groups ] || \
   [ -z "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]; then
    cat >&2 <<'EOF'
The IOMMU is not on, so VFIO has nothing to isolate the device with.

  Add to the kernel command line and reboot:

      intel_iommu=on iommu=pt

  On a machine booted by systemd-boot that is a line in
  /boot/loader/entries/*.conf; under GRUB it is GRUB_CMDLINE_LINUX_DEFAULT in
  /etc/default/grub, followed by grub-mkconfig -o /boot/grub/grub.cfg.

  Check it took with:  ls /sys/kernel/iommu_groups
EOF
    exit 1
fi

if [ ! -e "$ROOT/build/wos.img" ] || [ ! -e "$ROOT/build/wos.iso" ]; then
    echo "build WOS first: make" >&2
    exit 1
fi

# Everything in the adapter's IOMMU group comes with it, because a group is the
# smallest thing that can be isolated.  On a chipset device that can be more
# than expected, and taking an unrelated function away from Linux is not
# something to do without noticing.
GROUP="$(basename "$(readlink -f "/sys/bus/pci/devices/$SLOT/iommu_group")")"
echo "IOMMU group $GROUP holds:"
for d in /sys/kernel/iommu_groups/"$GROUP"/devices/*; do
    dev="$(basename "$d")"
    printf '  %s  %s\n' "$dev" "$(lspci -s "$dev" 2>/dev/null | cut -d' ' -f2- || echo '?')"
done

others="$(ls /sys/kernel/iommu_groups/"$GROUP"/devices/ | grep -cv "^$SLOT\$" || true)"
if [ "$others" -gt 0 ]; then
    echo
    echo "note: the group holds devices besides the adapter.  All of them must"
    echo "      be bound to vfio-pci for the passthrough to work, and Linux"
    echo "      loses all of them.  This script only moves the adapter; if"
    echo "      QEMU refuses the group, that is why."
fi

# ---- hand it over -----------------------------------------------------------

modprobe vfio-pci

current="$(basename "$(readlink -f "/sys/bus/pci/devices/$SLOT/driver" 2>/dev/null)" 2>/dev/null || echo none)"

if [ "$current" != "vfio-pci" ]; then
    echo "taking $SLOT from $current"
    if [ "$current" != "none" ]; then
        echo "$SLOT" > "/sys/bus/pci/devices/$SLOT/driver/unbind"
    fi
    echo "vfio-pci" > "/sys/bus/pci/devices/$SLOT/driver_override"
    echo "$SLOT" > /sys/bus/pci/drivers_probe
fi

bound="$(basename "$(readlink -f "/sys/bus/pci/devices/$SLOT/driver" 2>/dev/null)" 2>/dev/null || echo none)"
if [ "$bound" != "vfio-pci" ]; then
    echo "could not bind $SLOT to vfio-pci (it is on $bound)" >&2
    exit 1
fi
echo "$SLOT is on vfio-pci; Linux has no wireless until --return"

# ---- run it -----------------------------------------------------------------
#
# The serial log is what to read afterwards: every line the driver prints goes
# there, and on a run that fails the last of them says how far it got.

LOG="${LOG:-$ROOT/build/passthrough.log}"

# Headless by design.  This runs under sudo, so QEMU is root and cannot open a
# window on the login session's display -- it fails with "gtk initialization
# failed", which says nothing about the wireless adapter and looks alarming.
# Everything the driver prints goes to the serial line anyway, which is the
# thing worth reading.
#
# Set WOS_DISPLAY=gtk to ask for a window regardless; it needs xhost or an
# XAUTHORITY that root can read, and is only worth it to watch the console.
DISPLAY_ARG="-display none"
[ "${WOS_DISPLAY:-}" = "gtk" ] && DISPLAY_ARG="-display gtk"

: > "$LOG"          # start clean, so what is read afterwards is from this run

echo "booting WOS headless; serial log -> $LOG"
echo "(it runs for ${WOS_SECONDS:-25}s, then stops)"
echo

# Bounded, because nothing here is going to type at it: the driver does its
# work during the boot and the log is what is left behind.
timeout "${WOS_SECONDS:-25}" \
qemu-system-x86_64 \
    -enable-kvm -cpu host -m 512M -smp 2 \
    ${WOS_MACHINE:+-machine $WOS_MACHINE} \
    -cdrom "$ROOT/build/wos.iso" \
    -drive file="$ROOT/build/wos.img",format=raw,if=ide,index=0,media=disk \
    -snapshot \
    -device vfio-pci,host="$SLOT" \
    -vga std \
    $DISPLAY_ARG \
    -serial "file:$LOG" \
    -no-reboot \
    "$@" || true

echo
if [ ! -s "$LOG" ]; then
    echo "the serial log is empty -- QEMU did not get far enough to boot WOS."
    echo "run it again without 'timeout' to see what it says."
else
    echo "what the driver said:"
    echo "---------------------------------------------------------------"
    grep -aE "iwlwifi|wifi   |net    |pci    " "$LOG" || \
        echo "(nothing from the wireless driver -- see $LOG for the whole boot)"
    echo "---------------------------------------------------------------"
fi
echo
echo "the whole boot log is in $LOG"
echo "give the adapter back with: sudo $0 --return"
