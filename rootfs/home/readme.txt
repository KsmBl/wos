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

Every application lives in /app/<name>/, with the executable at
/app/<name>/launch and its source in /app/<name>/sourcecode.
Typing a bare command name looks it up there.
