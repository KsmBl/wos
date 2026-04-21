Welcome to WOS.

This file lives on a real disk image in the WFS filesystem, not in RAM.
Anything you write here survives a reboot.

Useful things to try in whell:

  ls            list the current directory
  ls -l /app    long listing, with sizes
  pwd           print the working directory
  cd /app       change directory
  free          show memory use
  df            show disk use

Every command lives in /app/<name>/, with the executable at
/app/<name>/launch and its source in /app/<name>/sourcecode.
Typing a bare command name looks it up there, so `ls` really does
run /app/ls/launch -- try `ls /app` to see them all.

The shell itself only has cd, exit and help built in. Everything
else is a separate program, which is why `ps` can list itself.
