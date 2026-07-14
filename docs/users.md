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
| `/services` | root, or a user with the `systemctleditor` role |
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
| `editfreq` | changing the processor's clock, with [`cpufreq`](apps.md#cpufreq) |
| `systemctleditor` | starting, stopping, enabling and disabling services, with [`systemctl`](apps.md#systemctl) — and writing the unit files in `/services` |

Root holds no roles and needs none: every check short-circuits on uid 0.

The first two are write access to a place in the filesystem. `editfreq` and
`systemctleditor` are not:
there is one processor clock and every process on the machine runs on it, so a
slow machine is slow for everybody and a fast one is hot for everybody, and
what the machine is running is likewise everybody's. They are roles for the
same reason the other two are — something a user should be able to be trusted
with individually, without being made root.

`systemctleditor` is also what `/services` checks for writing, so the two ways
to change what runs at boot — `systemctl enable`, and editing the unit file —
need the same permission. A role that guarded only one of them would not be
guarding anything.

### The screen and the keyboard have no role

Taking the framebuffer or the keyboard — which is what a compositor like
[`sway`](apps.md#sway) does — needs **root**, and there is no role that grants
it.

That is deliberate rather than an omission. A role is for something one user
can be trusted with without being made root, and this is not that: there is one
screen and one keyboard, and a program holding either has taken it from
everybody at once. It can put anything it likes on the display, including a
convincing login prompt, and it sees every key anybody types. Handing that out
short of root would be handing out the machine.

### The seat, and the one way past that rule

A rule that strict would mean only root could ever have a desktop, which is not
a system anybody wants. So there is exactly one way for a program that is not
root to take the screen, and it goes through root either way: **root may arm a
grant, and the next process it spawns collects it.**

```c
wseatgrant();                            /* while still root      */
wlogin("bob", password);                 /* now uid 1, no way back */
wspawn("/app/sway/launch", argv);        /* takes the seat with it */
```

The screen and the keyboard travel together — a compositor with a display and
no keys is not a session anyone could use — and the pair of them is what is
called a **seat**.

That shape is forced by the order the work has to happen in.
[`login`](apps.md#login) starts as root, checks a password, becomes the user
who gave it, and starts their session; but a process that drops to a user can
never climb back, so by the time it has somebody to hand the seat to it is no
longer anybody who could grant one. Hence granting in advance.

What the grant does not do is widen who may take the screen:

- only root may arm it;
- it is spent by **one** spawn, and a second spawn gets nothing;
- it does not descend. The session leader has the seat; the terminals, editors
  and file managers it goes on to start are ordinary processes, and a program
  running inside your desktop cannot take the display away from the compositor
  drawing it.

So the machine still has exactly one path to the screen that does not begin at
root, and it runs through a program root chose to start. This is what
`systemd-logind` does for `sddm` and `lightdm`, with the parts that need a
session bus left out.

### `/ramdisk` is writable by everyone

A session needs somewhere to put its socket and its log, and a session is not
root, so the in-memory filesystem is writable by every user. It is this
system's `/tmp`, and it makes the same bargain — stated here rather than
discovered later:

- nothing in `/ramdisk` is private;
- with no per-file ownership, nothing there is safe from being overwritten by
  another account on the machine — including a compositor's socket, so a
  session's address is only as trustworthy as the accounts that exist;
- none of it survives a reboot, which is the part that limits the damage.

Every other path keeps the rules in the table above.

## Passwords

The user database lives under `/userconfig`:

```
/userconfig/users              the list of accounts:  name:uid:roles[:shell]
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

## Root's password is `1234`

A fresh image has one account, `root`, and its password is **`1234`**. That is
what the [login screen](apps.md#login) wants the first time the machine boots.

```
Choose an account
        root
   administrator
   [ 1234       ]
```

**It is a known default, published in this file, and every WOS image has the
same one.** Change it on any machine that other people can reach:

```
root@wos:/home/root# passwd
Changing password for root
New password:
Retype new password:
Password updated.
```

The alternative was shipping an account with no password at all, which is not
better — it is the same door, without even a number on it. What a known default
buys is that the login screen has something to check, so the mechanism is
exercised from the first boot rather than being a code path nobody meets until
they set a password by hand.

An account with no password can be entered by anyone who names it: at the login
screen, pressing Enter on an empty field lets them in. Setting a password to
the empty string deliberately returns an account to that state, and `passwd`
says so when it happens.

The same `1234` is used in one other place. If `/userconfig/users` is lost, the
kernel recreates `root` with it — that path is a machine being recovered rather
than installed, and a recovery that ends at a login screen nobody can get past
is not a recovery.

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
| `chsh [shell]`, `chsh -u <user> <shell>` | change a login shell; your own, or anyone's with `-u` if permitted |
| `adduser [-a] [-u] [-f] [-s] <name>` | create a user, asking for a password; `-a` grants appeditor, `-u` usereditor, `-f` editfreq, `-s` systemctleditor |
| `edituser <name> [+role] [-role]` | add or remove roles; with no change, prints what they hold |

Each user has a **login shell** — the program started for them by `su`, and by
[`login`](apps.md#login) when a text session is asked for with F2. It is a
fourth field on their line in `/userconfig/users`
(`name:uid:roles:shell`), defaulting to `whell`, and is changed with
[`chsh`](apps.md#chsh). Unlike the password, it is not a secret, so it lives in
the list rather than a protected file.

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
- **No sessions in parallel.** There is one screen and one keyboard, so there
  is one session. [`login`](apps.md#login) does not offer to switch users while
  somebody is logged in; you end the session and the login screen comes back.
- **No audit trail.** Nothing records who did what.

Adding per-file ownership is the natural next step, and it needs a bigger
inode — which means a filesystem version bump, since the on-disk format is
shared with the host `mkwfs` tool.
