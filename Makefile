CC       = gcc
AS       = as
LD       = ld
LWIP_DIR = /home/fierce/Projects/lwip
CFLAGS   = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
           -m32 -Wall -Wextra -std=c17 \
           -fno-pic -no-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -O0 \
           -fno-builtin -fno-omit-frame-pointer -I net -I include -I . -I $(LWIP_DIR)/src/include
ASFLAGS  = --32
LDFLAGS  = -T boot/linker.ld -nostdlib -m elf_i386

KERNEL_DIRS = kernel interrupts drivers mm proc fs lib boot net
SRCS_C  = $(wildcard $(addsuffix /*.c,$(KERNEL_DIRS)))
SRCS_S  = $(wildcard $(addsuffix /*.s,$(KERNEL_DIRS)))
OBJS    = $(SRCS_C:.c=.o) $(SRCS_S:.s=.o)

KALLSYMS_DATA_C = kallsyms_data.c
KALLSYMS_DATA_O = kallsyms_data.o

CMD_NAMES = pwd getpid clear ls kill cat date sleep uname stat mkdir rm echo touch cp mv ps whoami id true false yes basename dirname wc seq head which test cmp hexdump hostname ln chmod chown chgrp free poweroff reboot curl socket_test bench
CMD_ELFS  = $(addsuffix .elf,$(addprefix cmd_,$(CMD_NAMES)))

LIBC_DIR = user/libc
LIBC     = $(LIBC_DIR)/libc.a
CRT0     = $(LIBC_DIR)/crt0.o
USER_CFLAGS = $(CFLAGS) -I $(LIBC_DIR)/include

.PHONY: all clean mkdisk run libc

all: libc kernel.elf init.elf shell.elf $(CMD_ELFS)

libc:
	$(MAKE) -C $(LIBC_DIR)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.s
	$(AS) $(ASFLAGS) -o $@ $<

INITRAMFS_ELF = initramfs_init.elf
INITRAMFS_BIN = initramfs_init.bin
INITRAMFS_EMBED = initramfs_embed.o

$(INITRAMFS_ELF): user/initramfs_init.c
	$(CC) $(USER_CFLAGS) -nostdlib -ffreestanding -c -o initramfs_init.o user/initramfs_init.c
	ld -Ttext=0x08000000 -nostdlib -m elf_i386 -o $@ initramfs_init.o

$(INITRAMFS_EMBED): $(INITRAMFS_ELF)
	$(LD) -r -b binary -m elf_i386 -o $@ $<

KALLSYMS_STUB_O = kallsyms_data_stub.o

$(KALLSYMS_STUB_O): kallsyms_data_stub.c
	$(CC) $(CFLAGS) -c -o $@ $<

kernel_pre.elf: $(OBJS) $(KALLSYMS_STUB_O) $(INITRAMFS_EMBED) boot/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(KALLSYMS_STUB_O) $(INITRAMFS_EMBED)

$(KALLSYMS_DATA_C): kernel_pre.elf tools/gen_kallsyms.py
	nm -n $< | python3 tools/gen_kallsyms.py > $@

$(KALLSYMS_DATA_O): $(KALLSYMS_DATA_C)
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS) $(INITRAMFS_EMBED) $(KALLSYMS_DATA_O) boot/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(INITRAMFS_EMBED) $(KALLSYMS_DATA_O)

init.elf: user/init.c $(LIBC) $(CRT0)
	$(CC) $(USER_CFLAGS) -c -o init.o user/init.c
	ld -Ttext=0x08000000 -nostdlib -m elf_i386 -o $@ $(CRT0) init.o $(LIBC)

shell.elf: user/shell.c $(LIBC) $(CRT0)
	$(CC) $(USER_CFLAGS) -c -o shell.o user/shell.c
	ld -Ttext=0x08000000 -nostdlib -m elf_i386 -o $@ $(CRT0) shell.o $(LIBC)

cmd_%.elf: user/cmd.c $(LIBC) $(CRT0)
	$(CC) $(USER_CFLAGS) -Wno-unused-function -DCMD_$(shell echo $* | tr a-z A-Z) -c -o cmd_$*.o user/cmd.c
	ld -Ttext=0x08000000 -nostdlib -m elf_i386 -o $@ $(CRT0) cmd_$*.o $(LIBC)

clean:
	rm -rf *.o *.elf *.bin iso_root os.iso cmd_*.o initramfs_*.o initramfs_*.bin initramfs_*.elf $(KALLSYMS_DATA_C) kernel_pre.elf $(KALLSYMS_STUB_O)
	find . -name '*.o' -type f -delete
	$(MAKE) -C $(LIBC_DIR) clean

check: kernel.elf
	grub-file --is-x86-multiboot2 kernel.elf

TEST_FILES = user/test.txt init.elf shell.elf $(CMD_ELFS)

mkdisk: init.elf shell.elf $(CMD_ELFS)
	./tools/mkdisk.sh

disk2.img:
	dd if=/dev/zero of=$@ bs=1M count=4 2>/dev/null
	mkfs.ext2 -b 1024 -F $@ 2>/dev/null
	@echo "disk2.img created (4 MB, ext2)"

run: kernel.elf $(TEST_FILES) disk2.img
	mkdir -p iso_root/boot/grub
	cp kernel.elf iso_root/boot/
	cp user/test.txt iso_root/boot/
	@echo 'set timeout=0' > iso_root/boot/grub/grub.cfg
	@echo 'set default=0' >> iso_root/boot/grub/grub.cfg
	@echo '' >> iso_root/boot/grub/grub.cfg
	@echo 'menuentry "my_kernel" {' >> iso_root/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kernel.elf' >> iso_root/boot/grub/grub.cfg
	@echo '    module2 /boot/test.txt test.txt' >> iso_root/boot/grub/grub.cfg
	@echo '    boot' >> iso_root/boot/grub/grub.cfg
	@echo '}' >> iso_root/boot/grub/grub.cfg
	grub-mkrescue -o os.iso iso_root
	qemu-system-i386 -machine pc -cdrom os.iso -hda disk.img -hdb disk2.img -serial stdio -no-reboot -m 64 -nic user,model=e1000
