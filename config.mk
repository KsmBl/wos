# Build settings.
#
# Edit this file to change how WOS is built; `make` reads it every time.  Every
# setting can also be given on the command line for a one-off build, which wins
# over what is written here:
#
#     make DISK_MB=16
#
# `tools/configure.sh` edits this file from a menu, `make config` prints what is
# currently set, and changing anything here rebuilds what depends on it.

# ---------------------------------------------------------------------------
# What goes into the system
# ---------------------------------------------------------------------------

# Run the self-tests at boot: the four blocks of [ok  ] lines, which take a few
# seconds and include a process that faults on purpose.  0 leaves them out of
# the build entirely, so the kernel does not carry the code.
SELFTEST ?= 1

# Size of the filesystem image, in MiB.  The installed system is about 4 MiB;
# the rest is room to write into.
#
# On a machine the kernel can read the stick of, this is just disk space.  On
# one it cannot -- no xHCI, or the stick behind a hub -- the whole image is
# loaded into RAM instead and stays there, and the size of it is memory the
# machine does not get back.
DISK_MB ?= 128

# Size of the kernel heap arena, in MiB.  Everything kmalloc() hands out comes
# from here: process control blocks, kernel stacks, file buffers, a whole
# executable image while it loads.  It is reserved at boot whether it is used or
# not.  4 is comfortable; below that, spawning several processes at once will
# start to fail.
KHEAP_MB ?= 8

# ---------------------------------------------------------------------------
# The QEMU targets: make run, make run-nox, make log
# ---------------------------------------------------------------------------

# Memory given to the virtual machine.  Worth raising to try a build the way a
# real machine will see it -- several of the bugs this kernel has had only
# appear above a gigabyte, where the firmware stops putting everything in low
# memory.
QEMU_MEM ?= 2048M

# How long `make log` lets the machine run before capturing the serial log.
# Long enough for the self-tests to finish and the shell to start.
TIMEOUT ?= 12
