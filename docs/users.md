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
| `/userconfig` | root, or a user with the `usereditor` role |
| `/userconfig/<name>/password` | **root only** — overrides the line above |
| `/home/<user>` | that user, and everything beneath it |
| anywhere else | root only |

Reading is looser, but not unrestricted:

| Path | Who may read |
|---|---|
| `/userconfig/<name>/password` | **root only** |
| `/home/<user>` | that user — a home directory is private |
| `/home` itself | everyone — you can see which accounts exist |
| anywhere else | everyone |

So a user can `cat /kernel/README.txt`, list `/app`, or read `/userconfig/users`
— the list of accounts is not a secret. Passwords and other people's home
directories are.

`/home` stays listable on purpose. Hiding it would protect nothing:
`/userconfig/users` already publishes every account name, so all it would cost
you is the ability to see that your own home exists.

```
bob@wos:/home/bob$ ls /home
bob/   root/
bob@wos:/home/bob$ ls /home/root
ls: /home/root: permission denied
bob@wos:/home/bob$ cat /home/root/readme.txt
cat: /home/root/readme.txt: permission denied
bob@wos:/home/bob$ cd /home/root
cd: /home/root: permission denied
```

Note that last one. `cd` checks read permission because standing in a
directory you cannot read is simply a way to reach its contents by relative
path afterwards. `stat` checks it too — a file's size and type are worth
hiding, not just its contents.

Note the ordering of those two `/userconfig` rules: the password rule is
checked *first*, so the more specific path wins. Written the other way round,
`usereditor` would silently have gained access to every password on the
system.

The check runs on the *resolved* path, after `.` and `..` have been collapsed,
so `/home/bob/../../kernel/x` is caught rather than slipping past a prefix
test.

## Roles

Roles are a bitmask, so a user can hold several.

| Role | Grants |
|---|---|
| `appeditor` | write access under `/app` — installing and editing programs |
| `usereditor` | write access to `/userconfig` — creating users, setting anyone's password, changing roles |

Root holds no roles and needs none: every check short-circuits on uid 0.

## Passwords

The user database lives under `/userconfig`:

```
/userconfig/users              the list of accounts:  name:uid:roles
/userconfig/alice/password     alice's password:      salt:hash
/userconfig/bob/password       bob's password
```

Giving each user their own subdirectory is what makes the split possible. A
`usereditor` may write `/userconfig` — that is how accounts are added and roles
changed — while the password files inside are root-only. So:

```
alice@wos:/home/alice$ edituser bob +appeditor      # allowed: usereditor
bob (uid 2): appeditor
alice@wos:/home/alice$ touch /userconfig/x          # allowed
alice@wos:/home/alice$ cat /userconfig/bob/password
cat: /userconfig/bob/password: permission denied
alice@wos:/home/alice$ passwd bob                   # allowed, via the kernel
Password updated.
```

Alice can *set* bob's password but cannot *read* it. Setting goes through the
kernel, which does the file I/O itself; reading would mean opening the file,
which the VFS refuses.

**The kernel owns these files.** No program reads or writes them — `passwd`,
`su`, `adduser` and `edituser` all work through syscalls, and the kernel
reaches the files through the filesystem directly, below the permission
layer.

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
ring 3. If you ever gave programs read access to the password files, you would want a
real KDF first.

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
| `adduser [-a] [-u] <name>` | create a user, asking for a password; `-a` grants appeditor, `-u` usereditor |
| `edituser <name> [+role] [-role]` | add or remove roles; with no change, prints what they hold |

`adduser` also creates `/home/<name>` and `/userconfig/<name>/password`. Names
become part of a path, so `/`, `:`, `.` and newlines are refused.

`edituser` replaces the whole role set in one call, so it reads the current
roles first and sends the result:

```
root@wos:/home/root# edituser bob
bob (uid 2): no roles
root@wos:/home/root# edituser bob +appeditor +usereditor
bob (uid 2): appeditor usereditor
root@wos:/home/root# edituser bob -usereditor
bob (uid 2): appeditor
```

Root's roles cannot be edited. Every check short-circuits on uid 0, so they
carry no meaning, and allowing it would only suggest otherwise.

## What this is not

- **No per-file ownership.** Two users with write access to the same directory
  can overwrite each other's files. WFS has no room in an inode for an owner.
- **Read restrictions cover only home directories and the password files.**
  Everything else on the disk is readable by everyone, including
  `/userconfig/users` and every program under `/app`.
- **No setuid, and no groups.** Roles fill the same niche as groups but are
  fixed at compile time rather than being data.
- **No locking on the database.** `edituser` reads the current roles and writes
  back the result, so two simultaneous edits would have one overwrite the
  other. With a single console there is no way to try.
- **No login prompt at boot.** The machine starts a shell as root. `su` is how
  you become anyone else.
- **No audit trail.** Nothing records who did what.

Adding per-file ownership is the natural next step, and it needs a bigger
inode — which means a filesystem version bump, since the on-disk format is
shared with the host `mkwfs` tool.
