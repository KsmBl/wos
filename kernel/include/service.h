/* Services: programs the system runs, rather than a person.
 *
 * Each one is described by a file in /services, named after the service:
 *
 *     /services/wayland
 *         description=the Wayland display server
 *         exec=/app/waylandd/launch
 *         enabled=1
 *
 * The description lives on the disk and the running state lives here, because
 * the kernel is the only place that knows whether a process is still there.
 *
 * Enabling a service does not start it, and starting one does not enable it.
 * They answer different questions -- "should this run at the next boot" and
 * "is it running now" -- and a manager that treats them as one is how a
 * machine ends up in a state nobody asked for.
 *
 * Reading the files goes through wfs_* directly, below the permission layer,
 * the same way the user database does: there is no process to check at boot,
 * and the permission that matters is checked at the syscall, where there is
 * one.
 */
#ifndef WOS_SERVICE_H
#define WOS_SERVICE_H

#include "types.h"
#include "wabi.h"

/* Read /services and start everything enabled.  Called once at boot, after
 * the filesystem is mounted and the scheduler is running. */
void service_init(void);

/* Copy up to `max` service records out, refreshing what is running first.
 * Returns how many were written. */
int service_list(wservice_t *out, int max);

/* Do one of the W_SVC_* actions to a service by name.  Returns 0, -W_ENOENT
 * if there is no such service, -W_EBUSY when starting one that is already
 * running or stopping one that is not, -W_EINVAL for an unknown action, or an
 * error from spawning it.
 *
 * Permission is not checked here.  This file has no process to check it
 * against; the syscall does. */
int service_control(uint32_t action, const char *name);

/* Notice that a service's process has gone, so `running` stops claiming
 * otherwise.  Called when any process exits; a pid that is not a service's is
 * ignored. */
void service_reap(int32_t pid, int32_t status);

/* One line for the boot log. */
void service_print_report(void);

#endif /* WOS_SERVICE_H */
