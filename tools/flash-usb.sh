#!/usr/bin/env bash
#
# flash-usb.sh -- write WOS to a USB stick so it boots on real hardware.
#
# The default mode does not copy an image onto the stick; it installs a
# bootable system on it:
#
#   * two MBR partitions: FAT32 for the loader, and the WFS volume itself
#   * GRUB in the MBR and the gap behind it, for BIOS/CSM machines
#   * WOS's own UEFI loader at /EFI/BOOT/BOOTX64.EFI, for UEFI machines
#   * /boot/kernel.elf and /boot/wos.img, the kernel and its filesystem
#
# That layout is what firmware expects from a USB disk.  Writing the hybrid ISO
# to the device instead (--mode iso) is quicker but weaker: it leaves a GPT
# behind a protective MBR with no active partition, which some BIOSes will not
# offer as a boot device, and on a UEFI machine it boots without a filesystem,
# since UEFI firmware reads FAT and the image is on the ISO9660 track.
#
# The stick boots both ways: BIOS machines through GRUB, UEFI machines through
# WOS's own loader, because GRUB cannot hand over to this kernel under UEFI at
# all (docs/usb.md).
#
# The kernel reads partition 2 through its own xHCI driver, so what is written
# there survives a power off.  /boot/wos.img is the fallback for a controller
# the driver cannot use: loaded into memory before the kernel starts -- by GRUB
# as a Multiboot module, or by the UEFI loader reading it off the volume -- and
# lost at power off.  It is deliberately small; the stick's own partition is
# where the space is.
#
#   usage: sudo tools/flash-usb.sh [options]
#
#     --device /dev/sdX   skip the menu and use this device
#     --mode install|iso|img   what to write (default: install)
#                           install  partition + GRUB + kernel + filesystem
#                           iso      dd the hybrid ISO over the whole device
#                           img      only the raw WFS image, at LBA 0 -- for a
#                                    real IDE/SATA disk, which the kernel's ATA
#                                    driver can read and write persistently
#     --iso PATH          ISO to write   (default: build/wos.iso)
#     --img PATH          image to write (default: build/wos.img)
#     --all               list every disk, not just removable/USB ones
#     --yes               do not ask for confirmation (dangerous)
#     -h, --help          this text
#
set -euo pipefail

ORIG_ARGS=("$@")
SELF=$(readlink -f "${BASH_SOURCE[0]}")
ROOT=$(cd "$(dirname "$SELF")/.." && pwd)

ISO="$ROOT/build/wos.iso"
IMG="$ROOT/build/wos.img"
MKWFS="$ROOT/build/mkwfs"

# The volume magic, read from the header that defines it rather than written
# out here.
#
# It used to be spelled "WFS1" in three places in this script, and stayed that
# way through two format changes -- so every check below passed a volume that
# no longer existed and failed every volume that did.  A verification step
# that goes stale silently is worse than no verification step, because it
# fails on correct output and sends you looking in the wrong place.  Taking the
# value from include/wfs.h means the next format change cannot do this again.
wfs_magic() {
    local hex
    hex=$(sed -n 's/^#define WFS_MAGIC  *0x\([0-9A-Fa-f]\{8\}\)u\?.*/\1/p' \
          "$ROOT/include/wfs.h" | head -1)
    [[ -n $hex ]] || die "cannot find WFS_MAGIC in include/wfs.h"

    # Stored little-endian, so the bytes come out in reverse order.
    printf '%b' "\\x${hex:6:2}\\x${hex:4:2}\\x${hex:2:2}\\x${hex:0:2}"
}

ROOTFS="$ROOT/build/root"
KERNEL="$ROOT/build/kernel.elf"
EFIAPP="$ROOT/build/BOOTX64.EFI"
GRUBCFG="$ROOT/grub/grub.cfg"
MODE=install
DEVICE=""
SHOW_ALL=0
ASSUME_YES=0

# How big the copy in /boot may be.
#
# That copy is the fallback: it is read into RAM before the kernel starts, on a
# machine whose USB controller the kernel cannot drive, and it stays in RAM for
# the whole boot.  So it costs its own size in memory, and it has a ceiling it
# cannot be seen above at all -- the loader has to place it under the 768 MiB
# mark, because that is the part of memory the kernel identity maps.
#
# build/wos.img is whatever DISK_MB says, which is meant for `make run` and can
# easily be gigabytes.  Copying that here made the loader fail to place it and
# the machine boot with no filesystem whatsoever.  A separate small one is made
# instead when the built image is too big; the real filesystem is partition 2,
# which gets the whole stick regardless.
FALLBACK_MB=64

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*"; }

# Worked out here rather than beside the other paths above, because it can
# fail and reporting a failure needs die(), which is defined just above.
WFS_MAGIC_STR="$(wfs_magic)"

# The comment header is the manual: print it back, minus the leading hashes.
usage() { awk 'NR>2 && /^#/ { sub(/^# ?/, ""); print; next } NR>2 { exit }' "$SELF"; exit 0; }

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case $1 in
        --device) DEVICE=${2:-}; shift 2 ;;
        --mode)   MODE=${2:-};   shift 2 ;;
        --iso)    ISO=${2:-};    shift 2 ;;
        --img)    IMG=${2:-};    shift 2 ;;
        --all)    SHOW_ALL=1;    shift ;;
        --yes)    ASSUME_YES=1;  shift ;;
        -h|--help) usage ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

case $MODE in
    install|iso|img) ;;
    *) die "--mode must be install, iso or img" ;;
esac

need=(lsblk sfdisk blockdev findmnt wipefs dd)
[[ $MODE == install ]] && need+=(mkfs.vfat grub-install mktemp)
[[ $MODE == iso ]] && need+=(cmp)
for t in "${need[@]}"; do
    command -v "$t" >/dev/null || die "missing required tool: $t"
done

# Everything below needs to open a block device for writing.
if [[ $EUID -ne 0 ]]; then
    command -v sudo >/dev/null || die "run this as root"
    info "-- re-running under sudo"
    exec sudo -- "$SELF" "${ORIG_ARGS[@]+"${ORIG_ARGS[@]}"}"
fi

# What actually goes to /boot/wos.img.  build/wos.img unless that is over the
# ceiling, in which case a small one built for the purpose.
BOOTIMG="$IMG"

case $MODE in
    install)
        for f in "$KERNEL" "$EFIAPP" "$IMG" "$GRUBCFG"; do
            [[ -f $f ]] || die "no $f -- run 'make' first"
        done
        [[ -d /usr/lib/grub/i386-pc ]] || \
            die "no /usr/lib/grub/i386-pc -- install GRUB's BIOS target (grub-pc / grub2-pc)"

        if (( $(stat -c %s "$IMG") > FALLBACK_MB * 1024 * 1024 )); then
            [[ -x $MKWFS ]] || die "$MKWFS is missing; run make first"
            [[ -d $ROOTFS ]] || die "$ROOTFS is missing; run make first"

            info "-- $(basename "$IMG") is $(numfmt --to=iec "$(stat -c %s "$IMG")"); building a ${FALLBACK_MB} MiB one for /boot"
            BOOTIMG=$(mktemp /tmp/wos-boot-XXXXXX.img)
            trap 'rm -f "$BOOTIMG"' EXIT
            "$MKWFS" "$BOOTIMG" "$FALLBACK_MB" "$ROOTFS" >/dev/null
        fi
        ;;
    iso) [[ -f $ISO ]] || die "no $ISO -- run 'make' first" ;;
    img) [[ -f $IMG ]] || die "no $IMG -- run 'make' first" ;;
esac

# ---------------------------------------------------------------------------
# Pick the device
# ---------------------------------------------------------------------------

# lsblk -r escapes spaces in the model string; put it back for display only.
unescape() { printf '%b' "${1//\\x20/ }"; }

candidates=()
while read -r path size type tran hotplug model; do
    [[ $type == disk ]] || continue
    if [[ $SHOW_ALL -eq 0 ]]; then
        [[ $tran == usb || $hotplug == 1 ]] || continue
    fi
    candidates+=("$path|$size|$tran|$(unescape "$model")")
done < <(lsblk -dnr -o PATH,SIZE,TYPE,TRAN,HOTPLUG,MODEL)

if [[ -n $DEVICE ]]; then
    [[ -b $DEVICE ]] || die "$DEVICE is not a block device"
    [[ $(lsblk -dnr -o TYPE "$DEVICE") == disk ]] || \
        die "$DEVICE is a partition; give the whole disk (e.g. /dev/sdb)"
else
    [[ ${#candidates[@]} -gt 0 ]] || \
        die "no removable disk found -- plug the stick in, or pass --all"

    info "Removable devices:"
    [[ $SHOW_ALL -eq 1 ]] && info "(--all: every disk is listed, including internal ones)"
    i=0
    for c in "${candidates[@]}"; do
        i=$((i + 1))
        IFS='|' read -r path size tran model <<<"$c"
        printf '  %d) %-12s %-8s %-5s %s\n' "$i" "$path" "$size" "${tran:--}" "$model"
        # Show what is on it, so a wrong pick is obvious before it is fatal.
        lsblk -nr -o PATH,SIZE,FSTYPE,LABEL,MOUNTPOINTS "$path" | tail -n +2 | \
            while read -r p psize fstype label mnt; do
                printf '       %-14s %-8s %-10s %-12s %s\n' \
                       "$p" "$psize" "${fstype:--}" "$(unescape "${label:--}")" "${mnt:-}"
            done
    done

    printf '\nDevice number (or q to quit): '
    read -r choice
    [[ $choice == q ]] && exit 0
    [[ $choice =~ ^[0-9]+$ ]] && [[ $choice -ge 1 && $choice -le ${#candidates[@]} ]] || \
        die "not a listed number: $choice"
    DEVICE=${candidates[$((choice - 1))]%%|*}
fi

# ---------------------------------------------------------------------------
# Refuse the obviously fatal targets
# ---------------------------------------------------------------------------

dev_name=$(basename "$DEVICE")

root_src=$(findmnt -no SOURCE / || true)
root_disk=$(lsblk -no PKNAME "$root_src" 2>/dev/null | head -1 || true)
[[ -n $root_disk && $root_disk == "$dev_name" ]] && \
    die "$DEVICE holds the running root filesystem"

while read -r mnt; do
    [[ -z $mnt ]] && continue
    case $mnt in
        /|/boot|/boot/efi|/home|/usr|/var|/nix|"[SWAP]")
            die "$DEVICE has a partition mounted at $mnt -- this is not a spare stick" ;;
    esac
done < <(lsblk -nro MOUNTPOINTS "$DEVICE")

dev_sectors=$(blockdev --getsz "$DEVICE")
dev_bytes=$((dev_sectors * 512))

# ---------------------------------------------------------------------------
# Confirm
# ---------------------------------------------------------------------------

echo
info "About to write to $DEVICE ($(numfmt --to=iec "$dev_bytes"))"
lsblk -o PATH,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS "$DEVICE" | sed 's/^/  /'
echo
case $MODE in
    install)
        img_bytes=$(stat -c %s "$BOOTIMG")
        (( dev_bytes >= img_bytes + 128 * 1024 * 1024 )) || \
            die "$DEVICE is too small: the filesystem alone is $(numfmt --to=iec "$img_bytes")"
        echo "  a FAT32 boot partition and a WFS partition WOS reads and writes"
        echo "  $KERNEL  -> /boot/kernel.elf   (BIOS: GRUB loads this)"
        echo "  $EFIAPP  -> /EFI/BOOT/BOOTX64.EFI   (UEFI: the firmware loads this)"
        echo "  $BOOTIMG  -> /boot/wos.img  ($(numfmt --to=iec "$img_bytes"), the fallback, loaded into RAM at boot)"
        ;;
    iso) echo "  $ISO  -> $DEVICE (whole device, hybrid ISO)" ;;
    img) echo "  $IMG  -> $DEVICE at LBA 0 (raw WFS volume, not bootable)" ;;
esac
echo
warn "everything currently on $DEVICE will be destroyed."

if [[ $ASSUME_YES -eq 0 ]]; then
    printf 'Type ERASE to continue: '
    read -r confirm
    [[ $confirm == ERASE ]] || die "aborted"
fi

# ---------------------------------------------------------------------------
# Write
# ---------------------------------------------------------------------------

# Anything the desktop auto-mounted would otherwise be writing behind our back.
while read -r part mnt; do
    [[ -n $mnt ]] || continue
    info "-- unmounting $part from $mnt"
    # -A because one partition can be mounted at several places at once.
    umount -A "$part" 2>/dev/null || umount "$part" || die "could not unmount $part"
done < <(lsblk -nro PATH,MOUNTPOINTS "$DEVICE" | tail -n +2)

settle() { sync; command -v udevadm >/dev/null && udevadm settle || true; }

# The node for partition N: sdb1, but nvme0n1p1 / mmcblk0p1.
part_node() {
    local n=$1
    for cand in "${DEVICE}${n}" "${DEVICE}p${n}"; do
        [[ -b $cand ]] && { echo "$cand"; return 0; }
    done
    return 1
}

info "-- wiping old signatures"
wipefs -a "$DEVICE" >/dev/null

case $MODE in
img)
    info "-- writing $(basename "$IMG") to $DEVICE"
    dd if="$IMG" of="$DEVICE" bs=4M conv=fsync status=progress
    settle
    magic=$(dd if="$DEVICE" bs=4 count=1 status=none | tr -d '\0')
    [[ $magic == "$WFS_MAGIC_STR" ]] || die "verification failed: no $WFS_MAGIC_STR superblock at LBA 0 (found ${magic:-nothing})"
    info "-- verified: WFS superblock at LBA 0"
    ;;

iso)
    info "-- writing $(basename "$ISO") to $DEVICE"
    dd if="$ISO" of="$DEVICE" bs=4M conv=fsync status=progress
    settle
    info "-- verifying"
    cmp -n "$(stat -c %s "$ISO")" "$ISO" "$DEVICE" || die "the ISO did not land intact"
    ;;

install)
    # One partition, starting at 1 MiB.  The gap between the MBR and it is
    # where GRUB's core.img goes: BIOS machines load it from there by sector
    # number, before any filesystem is readable.
    #
    # Type ef (EFI System) with the active flag set is the combination firmware
    # actually accepts from a removable disk: BIOSes that insist on an active
    # partition find one, and UEFI accepts an ESP on an MBR label for removable
    # media.  A GPT would be tidier but is what several BIOSes refuse to boot.
    #
    # A second partition holds the filesystem itself, as a raw WFS volume with
    # no filesystem in front of it.  That is the one WOS actually runs on: it
    # reads the stick through its own USB driver, so what is written there is
    # still there at the next boot.  It cannot live inside the FAT partition --
    # WOS has no FAT driver, and the copy in /boot is only what the loader
    # hands over on a machine whose USB the kernel cannot drive.
    img_sectors=$(( ($(stat -c %s "$BOOTIMG") + 511) / 512 ))
    esp_sectors=$(( img_sectors + 65536 ))          # the image, plus room

    total_sectors=$(blockdev --getsz "$DEVICE")
    need_sectors=$(( 2048 + esp_sectors + 8192 ))
    if (( total_sectors < need_sectors )); then
        die "$DEVICE holds $((total_sectors/2048)) MiB; this needs $((need_sectors/2048)) MiB"
    fi

    # Partition 2 gets everything left, and the filesystem is made to fit it --
    # the stick's size is the disk's size, rather than whatever DISK_MB the
    # build happened to use.  sfdisk fills the rest of the device when a
    # partition is given no size.
    data_sectors=$(( total_sectors - 2048 - esp_sectors ))
    data_mb=$(( data_sectors / 2048 ))

    info "-- partitioning $DEVICE"
    sfdisk --wipe always --wipe-partitions always --quiet "$DEVICE" <<EOF
label: dos
unit: sectors
start=2048, size=$esp_sectors, type=ef, bootable
start=$(( 2048 + esp_sectors )), type=83
EOF
    settle
    partprobe "$DEVICE" >/dev/null 2>&1 || true
    settle

    part=$(part_node 1) || die "the kernel did not create a node for partition 1"
    datapart=$(part_node 2) || die "the kernel did not create a node for partition 2"

    info "-- formatting $part as FAT32"
    mkfs.vfat -F 32 -n WOS "$part" >/dev/null

    mnt=$(mktemp -d)
    cleanup() {
        mountpoint -q "$mnt" 2>/dev/null && umount "$mnt" || true
        rmdir "$mnt" 2>/dev/null || true
        [[ $BOOTIMG == "$IMG" ]] || rm -f "$BOOTIMG"
    }
    trap cleanup EXIT

    mount "$part" "$mnt"
    mkdir -p "$mnt/boot/grub"

    info "-- copying the kernel and its filesystem"
    cp "$KERNEL" "$mnt/boot/kernel.elf"
    cp "$BOOTIMG" "$mnt/boot/wos.img"
    cp "$GRUBCFG" "$mnt/boot/grub/grub.cfg"

    info "-- installing GRUB for BIOS"
    grub-install --target=i386-pc --boot-directory="$mnt/boot" \
                 --recheck "$DEVICE" 2>&1 | sed 's/^/   /'

    # No GRUB on the UEFI side: it cannot hand over to this kernel on current
    # firmware at all (docs/usb.md).  WOS's own loader goes where the firmware
    # looks for a removable disk's boot file, and loads the kernel itself.
    info "-- installing the UEFI loader"
    mkdir -p "$mnt/EFI/BOOT"
    cp "$EFIAPP" "$mnt/EFI/BOOT/BOOTX64.EFI"

    # Made on the partition rather than copied onto it: an image is a fixed
    # size and the partition is whatever is left of the stick, which is usually
    # a great deal more.  Only the metadata and the installed system are
    # written; the free space is blocks nobody has touched.
    info "-- making the filesystem on $datapart ($data_mb MiB)"
    [[ -x $MKWFS ]] || die "$MKWFS is missing; run make first"
    [[ -d $ROOTFS ]] || die "$ROOTFS is missing; run make first"
    "$MKWFS" "$datapart" "$data_mb" "$ROOTFS" | sed 's/^/   /'
    sync

    sync
    info "-- verifying"
    [[ -f $mnt/boot/kernel.elf ]]        || die "kernel.elf did not land"
    [[ -f $mnt/boot/wos.img ]]           || die "wos.img did not land"
    [[ -f $mnt/EFI/BOOT/BOOTX64.EFI ]]   || die "the UEFI loader did not land"
    [[ -f $mnt/boot/grub/i386-pc/normal.mod ]] || \
        die "GRUB's BIOS modules are missing from the stick"
    magic=$(dd if="$mnt/boot/wos.img" bs=4 count=1 status=none | tr -d '\0')
    [[ $magic == "$WFS_MAGIC_STR" ]] || die "wos.img on the stick is not a $WFS_MAGIC_STR volume (found ${magic:-nothing})"
    magic=$(dd if="$datapart" bs=4 count=1 status=none | tr -d '\0')
    [[ $magic == "$WFS_MAGIC_STR" ]] || die "$datapart is not a $WFS_MAGIC_STR volume (found ${magic:-nothing})"
    # 0x80 in the first partition entry: the active flag some BIOSes require.
    boot_flag=$(dd if="$DEVICE" bs=1 skip=446 count=1 status=none | od -An -tx1 | tr -d ' ')
    [[ $boot_flag == 80 ]] || warn "partition 1 is not marked active (flag $boot_flag)"
    df -h "$mnt" | tail -1 | sed 's/^/   /'

    umount "$mnt"
    trap - EXIT
    cleanup
    ;;
esac

settle
echo
info "Done."
lsblk -o PATH,SIZE,TYPE,PARTTYPENAME,FSTYPE,LABEL "$DEVICE" | sed 's/^/  /'

# ---------------------------------------------------------------------------
# What to expect on the other side
# ---------------------------------------------------------------------------

cat <<'NOTES'

Booting it
  Pick the stick from the firmware's boot menu.  Both entries work: the plain
  one boots through GRUB, the one shown as "UEFI: <stick>" boots through WOS's
  own UEFI loader.  Neither needs anything selected -- GRUB's menu timeout is
  zero and the UEFI loader has no menu.

  Secure Boot must be off; the UEFI loader is unsigned.

Where the filesystem comes from
  Partition 2 is the real one: the kernel drives the stick's xHCI controller
  itself, reads and writes that partition, and what is written there is still
  there next time.  The boot log says so:

      wfs    : mounted from the USB device, ...

  If the controller is one the driver cannot use, it falls back to the copy in
  /boot, read into RAM before the kernel starts:

      wfs    : mounted in RAM (changes are not saved), ...

  Everything works the same in that case -- the apps are all there -- but
  nothing written survives a reboot, and the boot log says which device got in
  the way.  A PATA/SATA disk in IDE/legacy (compatibility) mode, not AHCI, is
  the third option, and the kernel prefers it over RAM:

      sudo tools/flash-usb.sh --mode img --device /dev/sdX   # erases that disk

If the firmware still will not boot it
  Check that "USB HDD" or the stick itself is enabled in the boot order, and
  try the one-off boot menu rather than the fixed order.  Turn Secure Boot off.
  Some firmware only boots USB from particular ports; try the others.

  If the WOS loader starts and stops, it says why on screen first: every
  failure it can detect prints a line beginning "WOS:".
NOTES
