/* Users, roles and password checking.
 *
 * The user database is owned by the kernel and lives at /etc/users.  No
 * program ever reads it: authentication, password changes and user creation
 * all happen through syscalls, and the kernel does the file I/O itself.
 *
 * That is deliberate.  A traditional Unix would let `passwd` and `su` read the
 * hashes and rely on setuid to let them write; WOS has no setuid, and keeping
 * the file unreachable is both simpler and stronger -- there is no hash for an
 * ordinary user to take away and attack offline.
 */
#ifndef WOS_USER_H
#define WOS_USER_H

#include "types.h"
#include "wabi.h"

/* Load /etc/users, creating it with a single passwordless root if it is
 * missing.  Called once during boot, after the filesystem is mounted. */
void user_init(void);

/* Look a user up by name or uid.  Returns 0 and fills `out`, or -W_ENOENT. */
int user_by_name(const char *name, wuser_t *out);
int user_by_uid(uint32_t uid, wuser_t *out);

/* Copy up to `max` user records into `out`. Returns how many were written. */
int user_list(wuser_t *out, int max);

/* Check `password` against the stored hash for `name`.
 * Returns 0 and fills `out` on success, -W_ENOENT if there is no such user,
 * or -W_EACCES if the password is wrong. */
int user_authenticate(const char *name, const char *password, wuser_t *out);

/* Set a user's password.
 *
 * `actor` is the uid asking.  Root and holders of W_ROLE_USERADMIN may set
 * anyone's password without knowing the old one; anyone else may set only
 * their own, and must supply it.
 *
 * Returns 0, -W_ENOENT, -W_EACCES (wrong old password or not permitted), or
 * an I/O error from writing the database back. */
int user_set_password(uint32_t actor, const char *name,
                      const char *old_password, const char *new_password);

/* Create a user with the given roles and password.  Only root and holders of
 * W_ROLE_USERADMIN may do this.  Also creates /home/<name>.
 * Returns the new uid, or a negative W_E* code. */
int user_add(uint32_t actor, const char *name, const char *password,
             uint32_t roles);

/* True if `uid` may act with `role`. Root always may. */
bool user_has_role(uint32_t uid, uint32_t role);

#endif /* WOS_USER_H */
