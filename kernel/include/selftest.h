/* Boot-time self-tests.
 *
 * These run on every boot rather than living in a separate test harness: an OS
 * has no unit-test runner to fall back on, and a broken allocator or a missing
 * timer tick is far cheaper to find here than three subsystems later.
 *
 * `make SELFTEST=0` builds without them, for a boot that goes straight to the
 * shell.  The tests then are not skipped but absent: selftest.c compiles to
 * nothing and the calls below vanish, so nothing of them is left in the kernel.
 */
#ifndef WOS_SELFTEST_H
#define WOS_SELFTEST_H

#include "types.h"

#ifdef WOS_NO_SELFTEST

static inline void selftest_interrupts(void) { }
static inline void selftest_memory(void)     { }
static inline void selftest_filesystem(void) { }
static inline void selftest_ramdisk(void)    { }
static inline void selftest_sockets(void)    { }
static inline void selftest_processes(void)  { }
static inline void selftest_crypto(void)     { }
static inline void selftest_network(void)    { }

#else

void selftest_interrupts(void);
void selftest_memory(void);

/* Exercises the disk and filesystem, and maintains /home/boots.txt as a
 * boot counter -- rebooting without rebuilding the image should show it
 * climbing, which is the proof that writes really reach the disk. */
void selftest_filesystem(void);

/* The in-memory filesystem: that it stores what is written, and that it holds
 * no more memory than the files in it need. */
void selftest_ramdisk(void);

/* Local sockets: listening, connecting, bytes in both directions, a descriptor
 * passed from one endpoint to the other, and what each end sees when the other
 * closes. */
void selftest_sockets(void);

/* Spawns the `hello` test program in ring 3: checks argument passing, exit
 * status, preemption between two processes, and that a faulting process is
 * killed without taking the system down. */
void selftest_processes(void);

/* The cryptography the wireless supplicant rests on, against the vectors
 * published with each algorithm: SHA-1, HMAC, PBKDF2, AES, RFC 3394 key
 * unwrap and CCM, plus the two WPA2 master keys the 802.11i annex gives.
 *
 * These are worth a boot's time because a wrong digest here does not crash
 * anything -- it produces a handshake an access point declines without ever
 * saying why, which is close to undebuggable from the far end. */
void selftest_crypto(void);

/* Ask the network for an address by DHCP, and check what comes back.
 *
 * This is the one piece of the wireless work that a machine with no wireless
 * adapter can still exercise, and the emulated network has a server on it
 * that answers -- so booting under QEMU runs the real four-message exchange
 * against a real server.  A network with no server is reported and skipped,
 * not failed. */
void selftest_network(void);

/* Deliberately dereference a null pointer to show the page-fault handler
 * working.  Panics by design; only call it as the last thing in a boot. */
void selftest_page_fault(void) __attribute__((noreturn));

#endif /* WOS_NO_SELFTEST */

#endif /* WOS_SELFTEST_H */
