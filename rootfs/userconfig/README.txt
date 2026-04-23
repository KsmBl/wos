The user database.

  users              the list of accounts: name:uid:roles
  <name>/password    that account's password: salt:hash

Writing anywhere in this directory needs the usereditor role. The password
files are the exception: only root and the kernel may touch them, for
reading as well as writing.

That split is the point of giving each user a subdirectory. A usereditor can
add accounts, set passwords and change roles -- all through the kernel, which
does the file I/O itself -- but cannot open a password file and read what is
already stored.

No program ever opens one, which is why passwd and su need no setuid.
