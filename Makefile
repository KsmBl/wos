# WOS build system.
#
#   make          build the kernel, the apps and the bootable ISO + disk image
#   make run      boot it in QEMU (VGA window + serial on stdio)
#   make run-nox  boot headless, serial only -- what the automated checks use
#   make debug    boot stopped, waiting for gdb on :1234
#   make clean    remove build/
#
# No cross-compiler is required: the host gcc builds 32-bit freestanding code
# with -m32, and GAS handles the assembly, so there is no dependency on nasm.

BUILD    := build
ISODIR   := $(BUILD)/isodir
KERNEL   := $(BUILD)/kernel.elf
ISO      := $(BUILD)/wos.iso
DISK     := $(BUILD)/wos.img
DISK_MB  := 64

CC      := gcc
LD      := ld
HOSTCC  := gcc

# -fno-pie/-fno-pic matter: this host's gcc defaults to PIE, and a position
# independent kernel triple-faults immediately at 1 MiB.
# The -mno-* flags keep gcc from emitting x87/SSE instructions, which would
# fault because the kernel never enables those units.
CFLAGS := -m32 -std=gnu11 -ffreestanding -O2 -g \
          -Wall -Wextra -Wno-unused-parameter \
          -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
          -DWOS_KERNEL -Ikernel/include -Iinclude
ASFLAGS := -m32 -g -Ikernel/include -Iinclude
LDFLAGS := -m elf_i386 -nostdlib -no-pie -z noexecstack

# Assembly objects get a .asm.o suffix so that a foo.c and a foo.S in the same
# directory cannot silently compile over each other's object file.
KSRC_C := $(shell find kernel -name '*.c' | sort)
KSRC_S := $(shell find kernel -name '*.S' | sort)
KOBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KSRC_C)) $(patsubst %.S,$(BUILD)/%.asm.o,$(KSRC_S))
KDEP   := $(KOBJ:.o=.d)

.PHONY: all kernel iso disk run run-nox log debug clean

all: iso disk

kernel: $(KERNEL)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.asm.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(KERNEL): $(KOBJ) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KOBJ)
	@# A kernel GRUB cannot recognise is the single most common early failure,
	@# so fail the build here rather than at boot.
	@grub-file --is-x86-multiboot $@ && echo "  multiboot header OK: $@"

iso: $(ISO)

$(ISO): $(KERNEL) grub/grub.cfg
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL) $(ISODIR)/boot/kernel.elf
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

MKWFS  := $(BUILD)/mkwfs
ROOTFS := $(BUILD)/root

$(MKWFS): tools/mkwfs.c include/wfs.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -o $@ $<

ROOTFS_SRC := $(shell find rootfs -type f 2>/dev/null)

disk: $(DISK)

$(DISK): $(MKWFS) $(ROOTFS_SRC)
	@rm -rf $(ROOTFS)
	@mkdir -p $(ROOTFS)
	@cp -r rootfs/. $(ROOTFS)/
	$(MKWFS) $@ $(DISK_MB) $(ROOTFS)

QEMU := qemu-system-i386
QEMU_FLAGS := -m 256M \
              -cdrom $(ISO) \
              -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
              -boot d -no-reboot

run: all
	$(QEMU) $(QEMU_FLAGS) -serial stdio

# Headless: no window, serial to stdout.
run-nox: all
	$(QEMU) $(QEMU_FLAGS) -serial stdio -display none

# Boot headless for TIMEOUT seconds and capture the serial log to a file.
# Piping `-serial stdio` through a timeout loses output to buffering, so the
# automated checks always go through a file instead.
TIMEOUT ?= 12
log: all
	@rm -f $(BUILD)/serial.log
	-@timeout $(TIMEOUT) $(QEMU) $(QEMU_FLAGS) \
	    -serial file:$(BUILD)/serial.log -display none >/dev/null 2>&1
	@cat $(BUILD)/serial.log

debug: all
	$(QEMU) $(QEMU_FLAGS) -serial stdio -display none -s -S

clean:
	rm -rf $(BUILD)

-include $(KDEP)
