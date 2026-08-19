#!/bin/sh
# Put the wireless adapter's firmware into the image.
#
# The adapter does nothing without it: about 1.45 MiB of signed code that the
# driver loads over DMA before the device has a MAC, a radio or a notion of a
# channel.
#
# This used to cut the firmware into six pieces, because WFS held at most
# 267 KiB in one file.  WFS grew a double-indirect block and now holds 64 MiB,
# so the firmware is one file again and the driver reads it in one go.
#
# The firmware is not in this repository and never will be: it is Intel's,
# redistributable but not ours to keep in a source tree.  It is taken from the
# machine doing the build, where Linux has already installed it.  A machine
# without it simply builds an image without wireless firmware, and the driver
# says so at boot rather than failing.
#
#   copyfw.sh <staging-dir>

set -eu

STAGE="${1:?usage: copyfw.sh <staging-dir>}"

DEST="$STAGE/lib/firmware"
NAME="iwlwifi-9000.ucode"

# Where Linux keeps it, newest revision first -- the adapter accepts a range of
# firmware versions and the newest is the one its own driver would have picked.
CANDIDATES="
/lib/firmware/intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-46.ucode
/lib/firmware/intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-38.ucode
/lib/firmware/intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-34.ucode
/lib/firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode
/lib/firmware/iwlwifi-9000-pu-b0-jf-b0-38.ucode
/lib/firmware/iwlwifi-9000-pu-b0-jf-b0-34.ucode
"

found=""
compressed=""

for f in $CANDIDATES; do
    if [ -f "$f" ]; then
        found="$f"; break
    fi
    if [ -f "$f.zst" ]; then
        found="$f.zst"; compressed="zstd"; break
    fi
    if [ -f "$f.xz" ]; then
        found="$f.xz"; compressed="xz"; break
    fi
done

if [ -z "$found" ]; then
    echo "  no iwlwifi 9000 firmware on this machine; the image will have none"
    echo "  (install linux-firmware to build an image that can use wireless)"
    exit 0
fi

mkdir -p "$DEST"
OUT="$DEST/$NAME"

case "$compressed" in
zstd)
    if ! command -v zstd >/dev/null 2>&1; then
        echo "  found $found but no zstd to decompress it; skipping firmware"
        exit 0
    fi
    zstd -dcq "$found" > "$OUT"
    ;;
xz)
    if ! command -v xz >/dev/null 2>&1; then
        echo "  found $found but no xz to decompress it; skipping firmware"
        exit 0
    fi
    xz -dc "$found" > "$OUT"
    ;;
*)
    cp "$found" "$OUT"
    ;;
esac

# The first four bytes are zero and the next four spell "IWL\n".  Checking here
# means a truncated or wrong file is caught by the build rather than by the
# driver on a machine with no console.
#
# Only the first three are compared: the fourth is a newline, and command
# substitution strips trailing newlines, so it cannot survive to be tested.
if [ "$(dd if="$OUT" bs=1 skip=4 count=3 status=none 2>/dev/null)" != "IWL" ]; then
    echo "  $found does not look like firmware (bad magic); skipping"
    rm -f "$OUT"
    exit 0
fi

echo "  wireless firmware: $(wc -c < "$OUT") bytes from $found"
