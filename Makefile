# WOS build system.
#
#   make          build the kernel, the apps and the bootable ISO + disk image
#   make run      boot it in QEMU (VGA window + serial on stdio)
#   make run-nox  boot headless, serial only -- what the automated checks use
#   make debug    boot stopped, waiting for gdb on :1234
#   make clean    remove build/
#
# No cross-compiler is required: the host gcc builds 64-bit freestanding code
# with -m64, and GAS handles the assembly, so there is no dependency on nasm.

BUILD    := build
ISODIR   := $(BUILD)/isodir
KERNEL   := $(BUILD)/kernel.elf
ISO      := $(BUILD)/wos.iso
DISK     := $(BUILD)/wos.img
DISK_MB  := 64

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
          -DWOS_KERNEL -Ikernel/include -Iinclude
ASFLAGS := -m64 -g -Ikernel/include -Iinclude
LDFLAGS := -m elf_x86_64 -nostdlib -no-pie -z noexecstack -n

# Assembly objects get a .asm.o suffix so that a foo.c and a foo.S in the same
# directory cannot silently compile over each other's object file.
KSRC_C := $(shell find kernel -name '*.c' | sort)
KSRC_S := $(shell find kernel -name '*.S' | sort)
KOBJ   := $(patsubst %.c,$(BUILD)/%.o,$(KSRC_C)) $(patsubst %.S,$(BUILD)/%.asm.o,$(KSRC_S))
KDEP   := $(KOBJ:.o=.d)

.PHONY: all kernel lib apps iso disk run run-nox log debug clean

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

# ---------------------------------------------------------------------------
# Applications
#
# Each app/<name>/sourcecode/*.c builds into $(BUILD)/app/<name>/launch, which
# the image installs at /app/<name>/launch alongside a copy of its source.
# ---------------------------------------------------------------------------

UCFLAGS := -m64 -std=gnu11 -ffreestanding -O2 -g \
           -Wall -Wextra -Wno-unused-parameter \
           -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables \
           -mcmodel=small -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
           -Ilib/wkernel/include -Iinclude
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

lib: $(LIBW)

# Only directories that actually contain sources; an empty app/<name>/ would
# otherwise produce a link with no input files.
APPS     := $(sort $(foreach f,$(wildcard app/*/sourcecode/*.c),\
                     $(word 2,$(subst /, ,$(f)))))
APP_BINS :=
APP_DEPS :=

define APP_RULES
APP_$(1)_SRC := $$(wildcard app/$(1)/sourcecode/*.c)
APP_$(1)_OBJ := $$(patsubst app/$(1)/sourcecode/%.c,$$(BUILD)/app/$(1)/%.o,$$(APP_$(1)_SRC))

$$(BUILD)/app/$(1)/%.o: app/$(1)/sourcecode/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(UCFLAGS) -MMD -MP -c $$< -o $$@

$$(BUILD)/app/$(1)/launch: $$(APP_$(1)_OBJ) $$(LIBW) lib/wkernel/user.ld
	@mkdir -p $$(dir $$@)
	$$(LD) $$(ULDFLAGS) -T lib/wkernel/user.ld -o $$@ $$(APP_$(1)_OBJ) $$(LIBW)
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

$(DISK): $(MKWFS) $(KERNEL) $(APP_BINS) $(ROOTFS_SRC) $(APP_SRC)
	@rm -rf $(ROOTFS)
	@mkdir -p $(ROOTFS)
	@cp -r rootfs/. $(ROOTFS)/
	@for a in $(APPS); do \
	    mkdir -p $(ROOTFS)/app/$$a/sourcecode; \
	    cp $(BUILD)/app/$$a/launch $(ROOTFS)/app/$$a/launch; \
	    cp app/$$a/sourcecode/* $(ROOTFS)/app/$$a/sourcecode/; \
	done
	@# /kernel gets the stripped binary and the source it came from. Stripped
	@# because the debug build is close to the 268 KiB per-file ceiling.
	@mkdir -p $(ROOTFS)/kernel/sourcecode
	@objcopy --strip-debug $(KERNEL) $(ROOTFS)/kernel/kernel.elf
	@find kernel include -name '*.c' -o -name '*.h' -o -name '*.S' \
	    | while read f; do cp "$$f" $(ROOTFS)/kernel/sourcecode/; done
	$(MKWFS) $@ $(DISK_MB) $(ROOTFS)

QEMU := qemu-system-x86_64
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
-include $(LIBW_DEP)
-include $(APP_DEPS)
