# Users, roles and permissions

WOS has users. Every process runs as one, `root` is uid 0 and may do anything,
and everyone else is limited to their own home directory unless a role says
otherwise.

```
root@wos:/home/root# whoami -v
user  : root (uid 0)
roles : root -- every permission, and write access everywhere

bob@wos:/home/bob$ whoami -v
user  : bob (uid 1)
roles : appeditor
write : /home/bob
        /app
        (everything else is read-only; /kernel is root only)
```

The prompt ends in `#` for root and `$` for everyone else — the old convention,
and a useful one: it says at a glance whether anything is protected from you.

## Where you may write

WFS inodes are 64 bytes and have no room for an owner or a mode, so permission
is decided by **path** rather than per file. Coarser than Unix, but it states
exactly the rules this system has:

| Path | Who may write |
|---|---|
| `/kernel` | root only — no role grants this |
| `/app` | root, or a user with the `appeditor` role |
| `/home/<user>` | that user, and everything beneath it |
| anywhere else | root only |

**Reading is unrestricted.** Any user can `cat /kernel/README.txt` or list
`/app`. The one thing worth hiding — the password file — is never handed to a
program at all.

The check runs on the *resolved* path, after `.` and `..` have been collapsed,
so `/home/bob/../../kernel/x` is caught rather than slipping past a prefix
test.

## Roles

Roles are a bitmask, so a user can hold several.

| Role | Grants |
|---|---|
| `appeditor` | write access under `/app` — installing and editing programs |
| `useradmin` | creating users, and setting anyone's password |

Root holds no roles and needs none: every check short-circuits on uid 0.

## Passwords

The user database lives at `/etc/users`, one record per line:

```
name:uid:roles:salt:hash
```

**The kernel owns it.** No program reads or writes it — `passwd`, `su` and
`useradd` all work through syscalls, and the kernel does the file I/O itself.

That is the design decision that matters here. A traditional Unix lets
`passwd` read the hashes and relies on setuid to let it write them back. WOS
has no setuid, and keeping the file unreachable is both simpler and stronger:
there is no hash for an ordinary user to take away and attack offline.

### On the hashing, honestly

Passwords are stored as a salted, 4096-round FNV-1a hash. **This is not a real
password hash.** bcrypt, scrypt and argon2 exist because a fast hash can be
brute-forced at billions of guesses per second, and iterating FNV a few
thousand times does not meaningfully change that.

What it does buy: the file holds something other than plaintext, and the
per-user salt means two people with the same password do not look identical.
Comparison is constant-time, so a rejection takes the same time whatever was
wrong with it.

What actually protects passwords here is that the file is unreachable from
ring 3. If you ever gave programs read access to `/etc/users`, you would want
a real KDF first.

The salt comes from the timer tick and an address, because WOS has no entropy
source. It varies between users, which is all a salt has to do, but it is not
unpredictable.

## Root starts with no password

A fresh image has one account, `root`, with no password set. There is no
sensible default to ship and a known one would be worse than none, so the
first thing to do is:

```
root@wos:/home/root# passwd
Changing password for root
New password:
Retype new password:
Password updated.
```

An account with no password can be entered by anyone who names it. Setting a
password to the empty string deliberately returns it to that state, and
`passwd` says so when it happens.

## Becoming another user

```
root@wos:/home/root# su bob
bob@wos:/home/bob$
```

`su` starts a **new** shell rather than changing the one you are in, because a
process can drop to another user but never climb back: there is no way to
regain root once given up. Leaving that shell with `exit` returns you to the
one you started from, still as whoever you were.

Root is not asked for a password when becoming someone else — it could set
that password to anything first, so asking would be theatre.

## Commands

| Command | What it does |
|---|---|
| `whoami [-v]` | print the current user; `-v` explains the roles and write access |
| `passwd [user]` | change your own password, or another's if permitted |
| `su [user]` | start a shell as another user (default `root`) |
| `useradd [-a] [-u] <name>` | create a user; `-a` grants appeditor, `-u` useradmin |

`useradd` also creates `/home/<name>`, since a user with nowhere to write is
not much of a user. Names become part of a path, so `/`, `:`, `.` and newlines
are refused.

## What this is not

- **No per-file ownership.** Two users with write access to the same directory
  can overwrite each other's files. WFS has no room in an inode for an owner.
- **No read restrictions.** Every user can read every file. Only the password
  database is protected, and only because it never leaves the kernel.
- **No setuid, and no groups.** Roles fill the same niche as groups but are
  fixed at compile time rather than being data.
- **No login prompt at boot.** The machine starts a shell as root. `su` is how
  you become anyone else.
- **No audit trail.** Nothing records who did what.

Adding per-file ownership is the natural next step, and it needs a bigger
inode — which means a filesystem version bump, since the on-disk format is
shared with the host `mkwfs` tool.
