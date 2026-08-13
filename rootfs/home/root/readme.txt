Welcome to WOS.

This file lives on a real disk image in the WFS filesystem, not in RAM.
Anything you write here survives a reboot.

Useful things to try in whell:

  ls            list the current directory
  ls -l /app    long listing, with sizes and dates
  pwd           print the working directory
  cd /app       change directory
  free          show memory use
  df            show disk use

There is a worked example for `make` in example/ beside this file:

  cd example
  make          builds report.txt, because it is not there yet
  make          says there is nothing to do
  touch greeting.txt
  make          builds it again, because what it is built from changed

Every command lives in /app/<name>/, with the executable at
/app/<name>/launch and its source in /app/<name>/sourcecode.
Typing a bare command name looks it up there, so `ls` really does
run /app/ls/launch -- try `ls /app` to see them all.

The shell itself only has cd, exit and help built in. Everything
else is a separate program, which is why `ps` can list itself.
