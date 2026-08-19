# WOS build system.
#
#   make          build the kernel, the apps and the bootable ISO + disk image
#   make run      boot it in QEMU (VGA window + serial on stdio)
#   make run-nox  boot headless, serial only -- what the automated checks use
#   make debug    boot stopped, waiting for gdb on :1234
#   make clean    remove build/
#
# Build settings live in config.mk -- edit that file, or override any of them
# for one build on the command line:
#
#   make menuconfig    pick the drivers, the programs and the settings
#   make SELFTEST=0    build without the boot-time self-tests
#   make DISK_MB=16    build a smaller filesystem image
#   make config        print what is currently set
#   tools/configure.sh pick them from a menu, then build
#
# No cross-compiler is required: the host gcc builds 64-bit freestanding code
# with -m64, and GAS handles the assembly, so there is no dependency on nasm.

# Settings, and their defaults if the file is missing.  config.mk uses ?=, so
# anything given on the command line still wins over it.
-include config.mk

SELFTEST ?= 1
DISK_MB  ?= 64
KHEAP_MB ?= 8
QEMU_MEM ?= 256M
TIMEOUT  ?= 12

# What is built in.  `make menuconfig` writes these; they can equally be set
# by hand here or on the command line, which is why they are ordinary
# variables and not a format of their own.
#
# A driver that is off is not compiled at all -- its source leaves the build
# and the call that would have started it is compiled away, so what is left is
# a kernel that has never heard of it.  The drivers not listed here are not
# optional: the console, the keyboard, the timer and the PCI bus are how the
# machine works at all.
DRIVERS := RTL8139 IWLWIFI XHCI ATA MOUSE BATTERY

CONFIG_RTL8139 ?= y
CONFIG_IWLWIFI ?= y
CONFIG_XHCI    ?= y
CONFIG_ATA     ?= y
CONFIG_MOUSE   ?= y
CONFIG_BATTERY ?= y

# Multicore.  Off means the other processors are never started and the machine
# runs on the one it booted on.
SMP ?= y

# Programs left off the disk, by name.  Everything in app/ is included unless
# it is named here, so a program added to the tree needs no mention.
APPS_OFF ?=

BUILD    := build
ISODIR   := $(BUILD)/isodir
KERNEL   := $(BUILD)/kernel.elf
ISO      := $(BUILD)/wos.iso
DISK     := $(BUILD)/wos.img

CC      := gcc
LD      := ld
HOSTCC  := gcc

# WOS is an x86-64 kernel. GRUB enters it in 32-bit protected mode, since that
# is all Multiboot 1 can do, and boot.S makes the jump to long mode.
#
# -fno-pie/-fno-pic matter: this host's gcc defaults to PIE, and a position
# independent kernel triple-faults immediately at 1 MiB.
# -mcmodel=small holds because the whole kernel sits inside the identity-mapped
# first 1 GiB, so every symbol address fits in 32 bits.
# -mno-red-zone is not optional in a kernel: an interrupt would otherwise land
# on top of the 128 bytes below rsp that a leaf function assumes are its own.
# The remaining -mno-* flags keep gcc from emitting x87/SSE instructions, which
# would fault because the kernel never enables those units.
CFLAGS := -m64 -std=gnu11 -ffreestanding -O2 -g \
          -Wall -Wextra -Wno-unused-parameter \
          -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -mcmodel=small -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
          -DWOS_KERNEL -DKHEAP_MB=$(KHEAP_MB) -Ikernel/include -Iinclude -I$(BUILD)/include
ifeq ($(SELFTEST),0)
CFLAGS += -DWOS_NO_SELFTEST
endif
ASFLAGS := -m64 -g -Ikernel/include -Iinclude
LDFLAGS := -m elf_x86_64 -nostdlib -no-pie -z noexecstack -n

# Assembly objects get a .asm.o suffix so that a foo.c and a foo.S in the same
# directory cannot silently compile over each other's object file.
KSRC_C := $(shell find kernel -name '*.c' | sort)
KSRC_S := $(shell find kernel -name '*.S' | sort)

# Leave out what is switched off.  The call sites are compiled away too, by
# the CONFIG_* defines in the generated header below, so nothing is left
# referring to a driver that is not there.
ifneq ($(CONFIG_RTL8139),y)
KSRC_C := $(filter-out kernel/drivers/rtl8139.c,$(KSRC_C))
endif
ifneq ($(CONFIG_IWLWIFI),y)
KSRC_C := $(filter-out kernel/drivers/iwlwifi/%,$(KSRC_C))
endif
ifneq ($(CONFIG_XHCI),y)
KSRC_C := $(filter-out kernel/drivers/xhci.c kernel/drivers/usbdisk.c,$(KSRC_C))
endif
ifneq ($(CONFIG_ATA),y)
KSRC_C := $(filter-out kernel/drivers/ata.c,$(KSRC_C))
endif
ifneq ($(CONFIG_MOUSE),y)
KSRC_C := $(filter-out kernel/drivers/mouse.c,$(KSRC_C))
endif
ifneq ($(CONFIG_BATTERY),y)
KSRC_C := $(filter-out kernel/drivers/battery.c,$(KSRC_C))
endif
# SMP is not a file that can be left out: smp.c also owns the lock the
# scheduler, the page tables and the interrupt path take, on one processor as
# much as on eight.  Switching it off means not starting the other processors,
# which is what CONFIG_SMP does at the one call site that matters.

# What the kernel sources see of all that: one generated header, included by
# everything, saying which of the optional pieces are present.
WOSCONFIG := $(BUILD)/include/wosconfig.h

KOBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KSRC_C)) $(patsubst %.S,$(BUILD)/%.asm.o,$(KSRC_S))
KDEP   := $(KOBJ:.o=.d)

.PHONY: all kernel lib apps efi iso disk run run-nox log debug clean config menuconfig

all: iso disk

kernel: $(KERNEL)

# Pick what goes into the build, and write it to config.mk.
menuconfig:
	@python3 tools/menuconfig.py

# What the next build will use, and where each value came from -- editing
# config.mk and forgetting to save it looks exactly like a build that ignored
# you, and this is how to tell the difference.
config:
	@echo "settings (config.mk, overridable on the command line)"
	@echo "  SELFTEST = $(SELFTEST)   $(if $(filter 0,$(SELFTEST)),no boot-time self-tests,self-tests run at boot)"
	@echo "  DISK_MB  = $(DISK_MB)   filesystem image size"
	@echo "  KHEAP_MB = $(KHEAP_MB)   kernel heap arena"
	@echo "  QEMU_MEM = $(QEMU_MEM)   memory for make run / make log"
	@echo "  TIMEOUT  = $(TIMEOUT)   seconds make log waits"

# Settings that change what is built without touching a single source file.
# Nothing in the dependency files knows about them, so make would happily keep
# output built the other way.  A stamp named after the current value stands in
# for it: change the value and the name no longer exists, which rebuilds
# whatever depends on it.
# The configuration is part of this too.  Without it, switching a driver off
# on the command line would change nothing that make can see -- no source file
# is newer, so nothing is rebuilt, and the build quietly keeps the driver.
CONFIG_SIG := $(shell echo '$(foreach d,$(DRIVERS),$(CONFIG_$(d)))$(SMP)$(APPS_OFF)' \
                      | cksum | cut -d' ' -f1)

KERNEL_STAMP := $(BUILD)/.build-selftest$(SELFTEST)-kheap$(KHEAP_MB)-cfg$(CONFIG_SIG)
DISK_STAMP   := $(BUILD)/.disk-$(DISK_MB)

$(KERNEL_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.build-*
	@touch $@

$(DISK_STAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.disk-*
	@touch $@

$(WOSCONFIG): config.mk Makefile $(KERNEL_STAMP)
	@mkdir -p $(dir $@)
	@{ echo "/* Generated from config.mk by the build -- do not edit."; \
	   echo " * 'make menuconfig' is what writes the settings behind it. */"; \
	   echo "#ifndef WOS_WOSCONFIG_H"; \
	   echo "#define WOS_WOSCONFIG_H"; \
	   $(foreach d,$(DRIVERS), \
	     echo "#define CONFIG_$(d) $(if $(filter y,$(CONFIG_$(d))),1,0)";) \
	   echo "#define CONFIG_SMP $(if $(filter y,$(SMP)),1,0)"; \
	   echo "#endif"; } > $@
	@echo "  configured: $(strip $(foreach d,$(DRIVERS),$(if $(filter y,$(CONFIG_$(d))),$(d))))$(if $(filter y,$(SMP)), SMP)"


$(BUILD)/%.o: %.c $(KERNEL_STAMP) $(WOSCONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.asm.o: %.S $(KERNEL_STAMP) $(WOSCONFIG)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(KOBJ) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KOBJ)
	@# A kernel GRUB cannot recognise is the single most common early failure,
	@# so fail the build here rather than at boot.
	@grub-file --is-x86-multiboot $@ && echo "  multiboot header OK: $@"

# ---------------------------------------------------------------------------
# The UEFI loader
#
# Under UEFI nothing loads WOS but WOS: GRUB cannot hand over to this kernel on
# current firmware (see docs/usb.md), and the one path it does support wants a
# UEFI application anyway.  So the kernel is built into one -- a PE32+ binary
# the firmware loads straight from the EFI system partition.
#
# No cross-compiler here either: the host gcc builds position independent
# freestanding code, ld links it as a shared object, and objcopy converts that
# to PE.  The stub must come out with no relocations at all, since nothing
# applies them at load time, so the build checks.
# ---------------------------------------------------------------------------

EFIAPP := $(BUILD)/BOOTX64.EFI

UCFLAGS_EFI := -m64 -std=gnu11 -ffreestanding -O2 -fpic -fno-plt \
               -Wall -Wextra -Wno-unused-parameter \
               -fno-stack-protector -fno-builtin -mno-red-zone \
               -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
               -fno-asynchronous-unwind-tables \
               -Iuefi -Ikernel/include -Iinclude

$(BUILD)/uefi/stub.o: uefi/stub.c $(KERNEL)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS_EFI) \
	    -DKERNEL_MEM_END=0x$$(nm $(KERNEL) | awk '/ __kernel_end$$/ { print $$1 }') \
	    -c $< -o $@

# The kernel image is embedded, so blob.S depends on it being built first.
$(BUILD)/uefi/blob.o: uefi/blob.S $(BUILD)/kernel.bin
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(BUILD)/kernel.bin: $(KERNEL)
	@objcopy -O binary $(KERNEL) $@

efi: $(EFIAPP)

$(EFIAPP): $(BUILD)/uefi/stub.o $(BUILD)/uefi/blob.o uefi/stub.lds
	$(LD) -shared -Bsymbolic -nostdlib -e efi_main -T uefi/stub.lds \
	    -o $(BUILD)/uefi/stub.so $(BUILD)/uefi/stub.o $(BUILD)/uefi/blob.o
	@# Nothing applies relocations to this image at load time, so any that
	@# survived the link would be silently wrong addresses at runtime.
	@if readelf -r $(BUILD)/uefi/stub.so | grep -q '^[0-9a-f]'; then \
	    echo "  ERROR: the UEFI stub has load-time relocations:"; \
	    readelf -r $(BUILD)/uefi/stub.so; exit 1; \
	fi
	@# A section with no contents cannot be carried into the PE, so anything
	@# left in one would be written past the end of the image the firmware
	@# allocates -- into memory belonging to somebody else.  uefi/stub.lds
	@# folds .bss into .data to prevent it; this checks that it worked.
	@if readelf -SW $(BUILD)/uefi/stub.so | \
	    awk '$$3 == "NOBITS" && $$7 != "000000" { found = 1 } END { exit !found }'; then \
	    echo "  ERROR: the UEFI stub has data outside the loaded image:"; \
	    readelf -SW $(BUILD)/uefi/stub.so | awk '$$3 == "NOBITS"'; exit 1; \
	fi
	objcopy -j .text -j .rodata -j .data -j .reloc \
	    -O efi-app-x86_64 --subsystem=10 $(BUILD)/uefi/stub.so $@
	@echo "  built $@"

iso: $(ISO)

# The disk image rides along as a Multiboot module.  That is what makes the ISO
# self-contained: booted from a USB stick, where the kernel has no driver for
# the boot device, GRUB loads the filesystem into memory for it.
# The ISO carries both paths: GRUB with the Multiboot kernel for BIOS, and the
# UEFI loader for firmware that boots the ISO the other way, which GRUB's EFI
# build chainloads into.
$(ISO): $(KERNEL) $(EFIAPP) $(DISK) grub/grub.cfg
	@mkdir -p $(ISODIR)/boot/grub $(ISODIR)/EFI/wos
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
	cp $(DISK) $(ISODIR)/boot/wos.img
	@# Not /EFI/BOOT: grub-mkrescue puts its own boot file there, on the EFI
	@# system partition it builds.  Its config chainloads this one.
	cp $(EFIAPP) $(ISODIR)/EFI/wos/BOOTX64.EFI
	cp grub/grub.cfg $(ISODIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISODIR) 2>/dev/null
	@echo "  built $@"

# ---------------------------------------------------------------------------
# Disk image
#
# mkwfs is a host tool sharing include/wfs.h with the kernel, so the format it
# writes and the format the kernel reads cannot drift apart.  The image is
# built from a staging tree assembled in $(ROOTFS): everything under rootfs/
# verbatim, plus each application installed as /app/<name>/launch with its
# source beside it in /app/<name>/sourcecode.
#
# Rebuilding the image discards anything WOS wrote to it, which is what you
# want from a build -- but it means the persistence check must reboot without
# running make in between.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Applications
#
# Each app/<name>/sourcecode/*.c builds into $(BUILD)/app/<name>/launch, which
# the image installs at /app/<name>/launch alongside a copy of its source.
# ---------------------------------------------------------------------------

# -nostdinc keeps the host's /usr/include out of a WOS program entirely: a
# program that compiles here has compiled against wkernel and lib/wlibc and
# nothing else, which is the only way to know the machine can build it too.
# What is still needed from the compiler is its freestanding headers --
# stddef.h, stdarg.h, limits.h, stdint.h -- which belong to gcc rather than to
# any C library, and which -isystem puts back.
GCC_INCLUDE := $(shell $(CC) -print-file-name=include)

UCFLAGS := -m64 -std=gnu11 -ffreestanding -O2 -g \
           -Wall -Wextra -Wno-unused-parameter \
           -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables \
           -mcmodel=small -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
           -nostdinc -isystem $(GCC_INCLUDE) \
           -Ilib/wkernel/include -Ilib/wlibc/include -Iinclude
# No -n here, unlike the kernel: letting the linker page-align the segments
# keeps text and data in separate LOAD headers, so the loader can account for
# them separately instead of seeing one merged RWX blob.
ULDFLAGS := -m elf_x86_64 -nostdlib -no-pie -z noexecstack -z max-page-size=4096

# ---------------------------------------------------------------------------
# lib/wkernel -- the application API every program links against
# ---------------------------------------------------------------------------

LIBW     := $(BUILD)/lib/libwkernel.a
LIBW_C   := $(wildcard lib/wkernel/src/*.c)
LIBW_S   := $(wildcard lib/wkernel/src/*.S)
LIBW_OBJ := $(patsubst lib/wkernel/src/%.c,$(BUILD)/lib/%.o,$(LIBW_C)) \
            $(patsubst lib/wkernel/src/%.S,$(BUILD)/lib/%.asm.o,$(LIBW_S))
LIBW_DEP := $(LIBW_OBJ:.o=.d)

$(BUILD)/lib/%.o: lib/wkernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Ilib/wkernel/src -MMD -MP -c $< -o $@

$(BUILD)/lib/%.asm.o: lib/wkernel/src/%.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -g -Ilib/wkernel/include -Iinclude -MMD -MP -c $< -o $@

$(LIBW): $(LIBW_OBJ)
	@mkdir -p $(dir $@)
	ar rcs $@ $(LIBW_OBJ)
	@echo "  built $@"

# ---------------------------------------------------------------------------
# lib/wlibc -- the hosted C library, over wkernel
#
# What a program ported from Unix expects to find and wkernel deliberately does
# not have: FILE and stdio, qsort and strtol, setjmp, errno.  It is called
# libc.a because that is the name a compiler looks for.
#
# It is a layer, not a second system: malloc, strlen and the formatter come
# from libwkernel.a, which this archive is always linked beside.
# ---------------------------------------------------------------------------

LIBC     := $(BUILD)/lib/libc.a
LIBC_C   := $(wildcard lib/wlibc/src/*.c)
LIBC_S   := $(wildcard lib/wlibc/src/*.S)
LIBC_OBJ := $(patsubst lib/wlibc/src/%.c,$(BUILD)/lib/wlibc/%.o,$(LIBC_C)) \
            $(patsubst lib/wlibc/src/%.S,$(BUILD)/lib/wlibc/%.asm.o,$(LIBC_S))
LIBC_DEP := $(LIBC_OBJ:.o=.d)

$(BUILD)/lib/wlibc/%.o: lib/wlibc/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/lib/wlibc/%.asm.o: lib/wlibc/src/%.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -g -MMD -MP -c $< -o $@

$(LIBC): $(LIBC_OBJ)
	@mkdir -p $(dir $@)
	ar rcs $@ $(LIBC_OBJ)
	@echo "  built $@"

lib: $(LIBW) $(LIBC)

# Only directories that actually contain sources; an empty app/<name>/ would
# otherwise produce a link with no input files.
APPS     := $(sort $(foreach f,$(wildcard app/*/sourcecode/*.c),\
                     $(word 2,$(subst /, ,$(f)))))
# ... minus the ones config.mk says to leave out.
APPS     := $(filter-out $(APPS_OFF),$(APPS))
APP_BINS :=
APP_DEPS :=

define APP_RULES
APP_$(1)_SRC := $$(wildcard app/$(1)/sourcecode/*.c)
APP_$(1)_OBJ := $$(patsubst app/$(1)/sourcecode/%.c,$$(BUILD)/app/$(1)/%.o,$$(APP_$(1)_SRC))

$$(BUILD)/app/$(1)/%.o: app/$(1)/sourcecode/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(UCFLAGS) -MMD -MP -c $$< -o $$@

$$(BUILD)/app/$(1)/launch: $$(APP_$(1)_OBJ) $$(LIBC) $$(LIBW) lib/wkernel/user.ld
	@mkdir -p $$(dir $$@)
	@# libc.a before libwkernel.a, because what it needs -- malloc, wwrite,
	@# the formatter -- is in there, and a static archive is searched once
	@# for what is undefined at the moment it is reached.  An application
	@# that uses nothing from either takes nothing from either.
	$$(LD) $$(ULDFLAGS) -T lib/wkernel/user.ld -o $$@ $$(APP_$(1)_OBJ) \
	    $$(LIBC) $$(LIBW)
	@echo "  built $$@"

APP_BINS += $$(BUILD)/app/$(1)/launch
APP_DEPS += $$(APP_$(1)_OBJ:.o=.d)
endef

$(foreach a,$(APPS),$(eval $(call APP_RULES,$(a))))

apps: $(APP_BINS)

MKWFS  := $(BUILD)/mkwfs
ROOTFS := $(BUILD)/root

$(MKWFS): tools/mkwfs.c include/wfs.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -o $@ $<

ROOTFS_SRC := $(shell find rootfs -type f 2>/dev/null)
APP_SRC    := $(shell find app -type f 2>/dev/null)

disk: $(DISK)

$(DISK): $(MKWFS) $(KERNEL) $(APP_BINS) $(LIBW) $(LIBC) $(ROOTFS_SRC) \
         $(APP_SRC) tools/appmakefile.sh $(DISK_STAMP)
	@rm -rf $(ROOTFS)
	@mkdir -p $(ROOTFS)
	@cp -r rootfs/. $(ROOTFS)/
	@# The mount point for the in-memory filesystem.  It is empty on the disk
	@# and always will be: everything written to /ramdisk goes to memory.
	@mkdir -p $(ROOTFS)/ramdisk
	@# Stripped on the way in, the same way the kernel is.  Debug information
	@# is for the build tree, where it is still there for gdb; on the disk it
	@# is dead weight -- sway is 88 KiB of program and 300 KiB with its
	@# symbols attached.  It used to be dead weight the 267 KiB per-file
	@# ceiling eventually refused; the ceiling is 64 MiB now, and stripping is
	@# worth doing on its own terms rather than to make things fit.
	@# Each application's source arrives with a Makefile that rebuilds it,
	@# written by tools/appmakefile.sh rather than by hand -- fifty-two
	@# hand-written ones are fifty-two chances for one to drift.  It is
	@# generated at every build, so a file added to an application is in its
	@# Makefile without anybody remembering to put it there.
	@for a in $(APPS); do \
	    mkdir -p $(ROOTFS)/app/$$a/sourcecode; \
	    objcopy --strip-debug $(BUILD)/app/$$a/launch $(ROOTFS)/app/$$a/launch; \
	    cp app/$$a/sourcecode/* $(ROOTFS)/app/$$a/sourcecode/; \
	    tools/appmakefile.sh $$a app/$$a/sourcecode \
	        > $(ROOTFS)/app/$$a/sourcecode/Makefile; \
	done
	@# /kernel gets the stripped binary and the source it came from. Stripped
	@# of debug information only: the symbol table is worth keeping for
	@# anything reading the binary on the machine itself, and since WFS grew a
	@# double-indirect block there is no longer a size reason to drop it.
	@mkdir -p $(ROOTFS)/kernel/sourcecode
	@objcopy --strip-debug $(KERNEL) $(ROOTFS)/kernel/kernel.elf
	@find kernel include -name '*.c' -o -name '*.h' -o -name '*.S' \
	    | while read f; do cp "$$f" $(ROOTFS)/kernel/sourcecode/; done
	@# The toolchain's own half: what a program on this machine would have to
	@# compile and link against.  A compiler running here can find `wprintf`
	@# only if something on the disk has heard of it, and until these two
	@# directories exist nothing has.
	@#
	@# Stripped for the same reason the binaries are: the archive is 347 KiB
	@# with its debug information and 117 KiB without.
	@mkdir -p $(ROOTFS)/lib $(ROOTFS)/include
	@objcopy --strip-debug $(LIBW) $(ROOTFS)/lib/libwkernel.a
	@objcopy --strip-debug $(LIBC) $(ROOTFS)/lib/libc.a
	@cp lib/wkernel/include/*.h $(ROOTFS)/include/
	@cp lib/wlibc/include/*.h $(ROOTFS)/include/
	@cp include/wabi.h $(ROOTFS)/include/
	@cp lib/wkernel/user.ld $(ROOTFS)/lib/user.ld
	@# The compiler's own headers -- stddef.h and the three beside it, which
	@# describe the language rather than the library and so belong to wcc.
	@# It looks here before /include.
	@mkdir -p $(ROOTFS)/lib/wcc/include
	@cp app/wcc/include/*.h $(ROOTFS)/lib/wcc/include/
	$(MKWFS) $@ $(DISK_MB) $(ROOTFS)

QEMU := qemu-system-x86_64
# Use hardware virtualisation when the host has it (a /dev/kvm), which runs WOS
# at native speed; without it QEMU falls back to emulation automatically.
KVM := $(shell test -w /dev/kvm 2>/dev/null && echo "-enable-kvm -cpu host")
# An RTL8139 on QEMU's user-mode (SLIRP) network gives the guest 10.0.2.15 with
# a gateway at 10.0.2.2 that answers ARP and ping -- what the net stack targets.
# How many processors the emulated machine has.  Four, because the point of
# starting them is lost on one and four fits on any host that can run this.
QEMU_CPUS ?= 4

QEMU_FLAGS := $(KVM) -m $(QEMU_MEM) -smp $(QEMU_CPUS) \
              -cdrom $(ISO) \
              -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
              -netdev user,id=net0 -device rtl8139,netdev=net0 \
              -vga std \
              -boot d -no-reboot

run: all
	$(QEMU) $(QEMU_FLAGS) -serial stdio

# Headless: no window, serial to stdout.
run-nox: all
	$(QEMU) $(QEMU_FLAGS) -serial stdio -display none

# Boot headless for TIMEOUT seconds and capture the serial log to a file.
# Piping `-serial stdio` through a timeout loses output to buffering, so the
# automated checks always go through a file instead.
log: all
	@rm -f $(BUILD)/serial.log
	-@timeout $(TIMEOUT) $(QEMU) $(QEMU_FLAGS) \
	    -serial file:$(BUILD)/serial.log -display none >/dev/null 2>&1
	@cat $(BUILD)/serial.log

debug: all
	$(QEMU) $(QEMU_FLAGS) -serial stdio -display none -s -S

# Check a built WOS by booting it: `make check`, or `make check ARGS=-l` to
# see the scenarios, or `make check ARGS="boot mv"` for some of them.
#
# Nothing here can be checked by compiling.  Whether a directory entry moves
# atomically, whether a socket refuses another user, how far the pointer goes
# for a given movement of the mouse -- each is a property of the machine while
# it runs, and each of them was wrong at least once.
check: all
	@python3 tools/check.py $(ARGS)

clean:
	rm -rf $(BUILD)

-include $(KDEP)
-include $(LIBW_DEP)
-include $(LIBC_DEP)
-include $(APP_DEPS)
