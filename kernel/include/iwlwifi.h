/* Intel Wireless-AC 9560, and the rest of the 9000 series.
 *
 * This is the adapter in the machine WOS was written on: a CNVi part, meaning
 * the radio itself lives in the chipset and what appears on the PCI bus at
 * 8086:9df0 is the controller half of it.  From the driver's side that makes
 * little difference -- it is programmed the same way as the discrete cards of
 * the same generation -- but it is why the device sits at 00:14.3 alongside
 * the other chipset functions rather than on a slot of its own.
 *
 * The adapter does almost nothing until it is given firmware.  On a cold
 * machine its registers answer, and that is all: there is no MAC, no radio
 * control, no notion of a channel.  Bringing it up means powering the device,
 * handing it about a megabyte and a half of signed firmware over DMA, waiting
 * for that firmware to say it is alive, and from then on talking to the
 * firmware rather than to the hardware -- every real operation is a command
 * placed in a ring for it to pick up.
 *
 * ---------------------------------------------------------------------------
 * On the state of this driver
 *
 * The firmware parsing here is checked against the real firmware file and is
 * known to be right.  Everything below that -- the register addresses, the
 * power-up sequence, the ring formats and the command identifiers -- is
 * written from the published description of the hardware and has never been
 * run against the silicon, because there is no emulator for this adapter and
 * the machine it is in could not be used to test it.  It is bring-up code: an
 * honest first attempt, not a working driver, and the first person to run it
 * should expect to find faults in it.  Where a value is one this author is
 * less sure of, the comment beside it says so.
 * ---------------------------------------------------------------------------
 */
#ifndef WOS_IWLWIFI_H
#define WOS_IWLWIFI_H

#include "types.h"
#include "wosconfig.h"

/* Look for the adapter, and if it is there, start it: power it up, load its
 * firmware, and hand it to the 802.11 layer through wifi_attach().
 *
 * Returns whether an adapter was found and brought up.  A machine without one
 * gets false and no complaint; a machine with one that would not start gets
 * false and an explanation on the console. */
#if CONFIG_IWLWIFI
bool iwl_init(void);
#else
/* Built without the driver: there is no adapter as far as anything here is
 * concerned, which is the same answer a machine without one gives. */
static inline bool iwl_init(void) { return false; }
#endif

/* Where the firmware lives on the WOS filesystem.  The build puts it there
 * from the machine doing the building; see tools/copyfw.sh.
 *
 * It was once six numbered pieces, because WFS held at most 267 KiB in one
 * file and this is 1.45 MiB.  WFS grew a double-indirect block for exactly
 * this reason and now holds 64 MiB, so it is one file again. */
#define IWL_FIRMWARE_PATH "/lib/firmware/iwlwifi-9000.ucode"

#endif /* WOS_IWLWIFI_H */
