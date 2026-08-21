# No default arch: a forgotten ARCH= must not silently produce an i386
# kernel.  Only arch-independent goals (clean, host tests) work without it.
ARCH ?=

ifndef ARCH
ifeq ($(filter clean clean-hosttest hosttest,$(MAKECMDGOALS)),)
$(error ARCH is not set - build explicitly: make ARCH=amd64 (or i386, arm64))
endif
endif

ifeq ($(ARCH),arm64)
    CROSS_COMPILE ?= aarch64-linux-gnu-
    CC = $(CROSS_COMPILE)gcc
    AS = $(CROSS_COMPILE)as
    LD = $(CROSS_COMPILE)ld
else
    CC = gcc
    AS = as
    LD = ld
endif

# BSD personality — always linked in.  The microkernel plus the BSD
# layer (vfs/proc/sys/init) build together from bsd/.
BSD_ROOT = bsd
BSD_DIRS = $(BSD_ROOT)/init $(BSD_ROOT)/proc $(BSD_ROOT)/sys $(BSD_ROOT)/vfs $(BSD_ROOT)/vfs/devfs \
           $(BSD_ROOT)/vfs/ext2 $(BSD_ROOT)/vfs/ufs $(BSD_ROOT)/signal $(BSD_ROOT)/tty $(BSD_ROOT)/uipc \
           $(BSD_ROOT)/drivers $(BSD_ROOT)/drivers/serial $(BSD_ROOT)/net $(BSD_ROOT)/net/arch \
           $(BSD_ROOT)/arch/i386 $(BSD_ROOT)/arch/amd64 $(BSD_ROOT)/arch/arm64

# lwIP — vendored in bsd/net/lwip/src (see tools/vendor_lwip.sh + bsd/net/README.md).
# Dirty upstream tree (doc/doxygen, contrib/*.sln, src/apps/http/fs/*.html) is
# never committed: vendor script copies only whitelist.  Build uses explicit
# LWIP_SRCS (from src/Filelists.mk), not wildcard over bsd/net/lwip.
LWIPDIR  = bsd/net/lwip/src
LWIP_AVAILABLE := $(wildcard $(LWIPDIR)/core/init.c)
ifeq ($(LWIP_AVAILABLE),)
  LWIP_SRCS :=
else
  LWIP_COREFILES = $(LWIPDIR)/core/init.c $(LWIPDIR)/core/def.c $(LWIPDIR)/core/dns.c \
                   $(LWIPDIR)/core/inet_chksum.c $(LWIPDIR)/core/ip.c $(LWIPDIR)/core/mem.c \
                   $(LWIPDIR)/core/memp.c $(LWIPDIR)/core/netif.c $(LWIPDIR)/core/pbuf.c \
                   $(LWIPDIR)/core/raw.c $(LWIPDIR)/core/stats.c $(LWIPDIR)/core/sys.c \
                   $(LWIPDIR)/core/altcp.c $(LWIPDIR)/core/altcp_alloc.c $(LWIPDIR)/core/altcp_tcp.c \
                   $(LWIPDIR)/core/tcp.c $(LWIPDIR)/core/tcp_in.c $(LWIPDIR)/core/tcp_out.c \
                   $(LWIPDIR)/core/timeouts.c $(LWIPDIR)/core/udp.c
  LWIP_CORE4FILES = $(LWIPDIR)/core/ipv4/acd.c $(LWIPDIR)/core/ipv4/autoip.c $(LWIPDIR)/core/ipv4/dhcp.c \
                    $(LWIPDIR)/core/ipv4/etharp.c $(LWIPDIR)/core/ipv4/icmp.c $(LWIPDIR)/core/ipv4/igmp.c \
                    $(LWIPDIR)/core/ipv4/ip4_frag.c $(LWIPDIR)/core/ipv4/ip4.c $(LWIPDIR)/core/ipv4/ip4_addr.c
  LWIP_NETIFFILES = $(LWIPDIR)/netif/ethernet.c
  # APIFILES not needed for NO_SYS=1 raw API; keep err.c for error strings
  LWIP_APIFILES = $(LWIPDIR)/api/err.c
  LWIP_SRCS = $(LWIP_COREFILES) $(LWIP_CORE4FILES) $(LWIP_NETIFFILES) $(LWIP_APIFILES)
endif

ifeq ($(ARCH),arm64)
    MK_DIRS = mk/vm mk/dev mk/lib mk/time mk/smp \
              mk/arch/arm64/boot mk/arch/arm64/kern mk/arch/arm64/intr \
              mk/arch/arm64/thread mk/arch/arm64/vm mk/arch/arm64/fdt \
              mk/arch/arm64/smp
else ifeq ($(ARCH),amd64)
    MK_DIRS = mk/vm mk/dev mk/lib mk/time mk/smp \
              mk/arch/amd64/boot mk/arch/amd64/intr \
              mk/arch/amd64/thread mk/arch/amd64/kern \
              mk/arch/amd64/vm mk/arch/amd64/acpi mk/arch/amd64/smp
    CC_ARCH = -m64 -mno-red-zone
    AS_ARCH = --64
    LD_ARCH = -m elf_x86_64
else
    MK_DIRS = mk/vm mk/dev mk/lib mk/time mk/smp \
              mk/arch/i386/boot mk/arch/i386/intr \
              mk/arch/i386/kern mk/arch/i386/thread \
              mk/arch/i386/vm mk/arch/i386/smp
    CC_ARCH = -m32
    AS_ARCH = --32
    LD_ARCH = -m elf_i386
endif

ifeq ($(ARCH),arm64)
    TEST_DIRS =
else
    TEST_DIRS = mk/tests mk/tests/vm mk/tests/ipc mk/tests/kernel
endif

ALL_DIRS = boot $(MK_DIRS) $(BSD_DIRS) $(TEST_DIRS)
BSD_INCLUDES = -I $(BSD_ROOT)/include
HOST_BSD_FLAGS = -I $(BSD_ROOT)/include -I $(BSD_ROOT)/vfs/ext2 -I $(BSD_ROOT)/vfs/ufs \
                 -I mk/tests/host \
                 -DHOST_TEST_EXT2 -DHOST_TEST_UFS
HOST_BSD_SRCS = mk/tests/host/host_ext2_test.c mk/tests/host/host_fs_test.c \
                mk/tests/host/host_ufs_test.c \
                $(BSD_ROOT)/vfs/ext2/ext2.c $(BSD_ROOT)/vfs/ufs/ufs.c

ifeq ($(ARCH),arm64)
    # arm64 faults on misaligned 128-bit loads/stores; keep GCC from
    # emitting them (e.g. va_copy in debug_printf with -O0).
    CFLAGS_X86 = -mstrict-align
else
    CFLAGS_X86 = -mno-sse -mno-mmx
endif

DEBUG   ?= 0

ifeq ($(DEBUG),1)
    DEBUG_FLAGS = -DCONFIG_DEBUG
endif

CFLAGS   = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
           -Wall -Wextra -std=c17 \
           -fno-pic -no-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables $(CFLAGS_X86) -O0 \
           -fno-builtin \
           $(CC_ARCH) $(DEBUG_FLAGS)

ASFLAGS  = $(AS_ARCH)
LDFLAGS  = -T mk/arch/$(ARCH)/boot/linker.ld -nostdlib $(LD_ARCH)

INCLUDES = -I mk/arch/$(ARCH)/include -I mk/include -I mk/dev/include -I mk/tests \
           -I include -I . $(BSD_INCLUDES) \
           -I bsd/net -I bsd/net/arch \
           $(if $(LWIP_AVAILABLE),-I $(LWIPDIR)/include -I $(LWIPDIR)/include/lwip,)


SRCS_C = $(wildcard $(addsuffix /*.c,$(ALL_DIRS))) $(LWIP_SRCS)
SRCS_S = $(wildcard $(addsuffix /*.s,$(ALL_DIRS)))
SRCS_S += $(wildcard $(addsuffix /*.S,$(ALL_DIRS)))
OBJS   = $(patsubst %.c,%.o,$(SRCS_C)) \
         $(patsubst %.s,%.o,$(filter %.s,$(SRCS_S))) \
         $(patsubst %.S,%.o,$(filter %.S,$(SRCS_S)))

# =====================================================================
# Userspace programs (drivers, init, etc.)
# Built with the native compiler for the target arch.
# =====================================================================

USER_CFLAGS = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
              -Wall -Wextra -std=c17 \
              -fno-pic -no-pie -fno-stack-protector \
              -fno-asynchronous-unwind-tables \
              $(CFLAGS_X86) \
              -O2 -fno-builtin $(CC_ARCH)

USER_INCLUDES = -I user

USER_LDFLAGS  = -nostdlib $(LD_ARCH)

# List of userspace ELFs to build.  Built for every ARCH — the ABI
# (libdriver.h / init.c) has x86, amd64 and aarch64 branches, and the
# kernel's ELF loader + exec path are arch-generic.
#
# user/drivers/example/ is kept in-tree as a driver skeleton for
# developers but is NOT linked into the root image — build it manually
# with `make user/drivers/example/example.elf` if you need it.
USER_PROGS = user/init/init.elf user/tests/sigexec/sigexec.elf \
             user/drivers/driverd/driverd.elf

.PHONY: all clean user

all: arc.elf user

user: $(USER_PROGS)

# Generic rule: compile userspace .c -> .o
user/%.o: user/%.c
	$(CC) $(USER_CFLAGS) $(USER_INCLUDES) -c -o $@ $<

# Example driver
user/drivers/example/example.elf: user/drivers/example/main.o user/drivers/libdriver.o user/drivers/driver.ld
	$(LD) $(USER_LDFLAGS) -T user/drivers/driver.ld -o $@ user/drivers/example/main.o user/drivers/libdriver.o

# driverd — device enumeration daemon (walks the kernel device framework)
user/drivers/driverd/driverd.elf: user/drivers/driverd/main.o user/drivers/libdriver.o user/drivers/driver.ld
	$(LD) $(USER_LDFLAGS) -T user/drivers/driver.ld -o $@ user/drivers/driverd/main.o user/drivers/libdriver.o

# Init program (PID 1)
user/init/init.elf: user/init/init.o user/rc/rcparse.o user/init/init.ld
	$(LD) $(USER_LDFLAGS) -T user/init/init.ld -o $@ user/init/init.o user/rc/rcparse.o

# exec signal-state probe
user/tests/sigexec/sigexec.elf: user/tests/sigexec/sigexec.o user/tests/sigexec/sigexec.ld
	$(LD) $(USER_LDFLAGS) -T user/tests/sigexec/sigexec.ld -o $@ user/tests/sigexec/sigexec.o

# lwIP is whitelisted and needs different warnings (upstream triggers -Wunused-parameter etc.)
bsd/net/lwip/%.o: CFLAGS += -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers
# bsd/net/ (our wrapper) also sees lwIP headers
bsd/net/%.o: CFLAGS += -Wno-unused-parameter

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

%.o: %.s
	$(AS) $(ASFLAGS) -o $@ $<

%.o: %.S
	$(AS) $(ASFLAGS) -o $@ $<

arc.elf: $(OBJS) mk/arch/$(ARCH)/boot/linker.ld root.img
	$(LD) $(LDFLAGS) -o $@ $(OBJS) -b binary root.img

# root.img — ufs initramfs with userspace binaries (all arches)
ROOT_IMG_DEPS = user/init/init.elf user/tests/sigexec/sigexec.elf \
                user/drivers/driverd/driverd.elf \
                tools/etc/rc/mounts.rc tools/etc/rc/services.rc

root.img: $(ROOT_IMG_DEPS) tools/mkfs_ufs.py
	@echo "  GEN     $@"
	python3 tools/mkfs_ufs.py $@ --add user/init/init.elf:/sbin/init --add user/tests/sigexec/sigexec.elf:/sbin/sigexec --add user/drivers/driverd/driverd.elf:/sbin/driverd --add tools/etc/rc/mounts.rc:/etc/rc/mounts.rc --add tools/etc/rc/services.rc:/etc/rc/services.rc

# Bootable disk image with MBR + 2 partitions (boot + root).
# CI-only: built by tools/qa/qemu-smoke.sh, not part of the kernel build.
.PHONY: disk.img
disk.img: arc.elf root.img tools/qa/mkdisk.sh
	tools/qa/mkdisk.sh

clean:
	find . -name '*.o' -delete
	rm -f arc.elf arc.iso arc.bin disk.img root.img
	rm -rf iso_root
	rm -f $(USER_PROGS) user/drivers/null/null.elf

# =====================================================================
# Host tests — compiled with the native compiler as a regular ELF
# =====================================================================

HOST_CC     ?= gcc
HOST_CFLAGS  = -Wall -Wextra -std=c17 -g -O0 \
               -I mk/include -I include -I . \
               -DHOST_TEST \
               $(HOST_BSD_FLAGS)

HOST_TEST_SRCS = mk/tests/host/host_test_main.c \
                 mk/tests/host/host_string_test.c \
                 mk/tests/host/host_bitmap_test.c \
                 mk/tests/host/host_elf_test.c \
                 mk/tests/host/host_rcparse_test.c \
                 user/rc/rcparse.c \
                 $(HOST_BSD_SRCS)

.PHONY: hosttest clean-hosttest

hosttest: host_test
	./host_test

host_test: $(HOST_TEST_SRCS)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^

clean-hosttest:
	rm -f host_test

# QEMU boot smoke test (any arch): tools/qa/qemu-smoke.sh
#   amd64/i386: boots a GRUB disk image (built by tools/qa/mkdisk.sh)
#   arm64:      boots arc.bin in QEMU virt

# arm64: QEMU only places the DTB in RAM when booting a raw binary
# (-kernel with an ELF gives x0=0 and no DTB), so the kernel is
# objcopy'd to a plain binary for the run target.
arc.bin: arc.elf
	$(CROSS_COMPILE)objcopy -O binary $< $@
