/* Users, roles and password checking.
 *
 * The database lives under /userconfig:
 *
 *     /userconfig/users              the list:  name:uid:roles
 *     /userconfig/<name>/password    that user's password: salt:hash
 *
 * A user with W_ROLE_USEREDITOR may write /userconfig, which is how accounts
 * are added and roles changed.  The password files inside it are root-only,
 * for reading as well as writing -- a usereditor can create a user and set a
 * password through these calls, but cannot open the file and read what is
 * already there.
 *
 * No program ever opens a password file.  The kernel reaches them through
 * wfs_* directly, below the permission layer, which is what lets passwd and su
 * work without setuid: there is nothing privileged in those programs to abuse.
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
 * `actor` is the uid asking.  Root and holders of W_ROLE_USEREDITOR may set
 * anyone's password without knowing the old one; anyone else may set only
 * their own, and must supply it.
 *
 * Returns 0, -W_ENOENT, -W_EACCES (wrong old password or not permitted), or
 * an I/O error from writing the database back. */
int user_set_password(uint32_t actor, const char *name,
                      const char *old_password, const char *new_password);

/* Create a user with the given roles and password.  Only root and holders of
 * W_ROLE_USEREDITOR may do this.  Also creates /home/<name> and the user's
 * directory under /userconfig.
 * Returns the new uid, or a negative W_E* code. */
int user_add(uint32_t actor, const char *name, const char *password,
             uint32_t roles);

/* Replace a user's roles outright.  Only root and holders of
 * W_ROLE_USEREDITOR may do this, and root's own roles cannot be edited --
 * they are meaningless, since every check short-circuits on uid 0.
 * Returns 0, -W_EPERM, or -W_ENOENT. */
int user_set_roles(uint32_t actor, const char *name, uint32_t roles);

/* True if `uid` may act with `role`. Root always may. */
bool user_has_role(uint32_t uid, uint32_t role);

/* Get a user's login shell (always a usable path -- the default if unset), or
 * set it.  A user may set their own; root and W_ROLE_USEREDITOR may set any.
 * user_set_shell returns 0, -W_ENOENT, -W_EPERM or -W_ENAMETOOLONG. */
int user_shell(uint32_t uid, char *out, size_t cap);
int user_set_shell(uint32_t actor, const char *name, const char *shell);

#endif /* WOS_USER_H */
