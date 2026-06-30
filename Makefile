CC       = gcc
AS       = as
LD       = ld
CFLAGS   = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
           -m32 -Wall -Wextra -std=c17 \
           -fno-pic -no-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -O0 \
           -fno-builtin -fno-omit-frame-pointer -I include
ASFLAGS  = --32
LDFLAGS  = -T boot/linker.ld -nostdlib -m elf_i386

KERNEL_DIRS = kernel interrupts drivers mm proc fs lib boot
SRCS_C  = $(wildcard $(addsuffix /*.c,$(KERNEL_DIRS)))
SRCS_S  = $(wildcard $(addsuffix /*.s,$(KERNEL_DIRS)))
OBJS    = $(SRCS_C:.c=.o) $(SRCS_S:.s=.o)

CMD_NAMES = pwd getpid clear ls kill cat date sleep uname stat mkdir rm echo touch cp mv ps whoami id true false yes basename dirname wc seq head which test cmp hexdump hostname
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

kernel.elf: $(OBJS) boot/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

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
	rm -rf *.o *.elf *.bin iso_root os.iso cmd_*.o
	find . -name '*.o' -type f -delete
	$(MAKE) -C $(LIBC_DIR) clean

check: kernel.elf
	grub-file --is-x86-multiboot2 kernel.elf

TEST_FILES = user/test.txt init.elf shell.elf $(CMD_ELFS)

mkdisk: init.elf shell.elf $(CMD_ELFS)
	./tools/mkdisk.sh

run: kernel.elf $(TEST_FILES)
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
	qemu-system-i386 -machine q35 -cdrom os.iso -drive file=disk.img,format=raw -serial stdio -no-reboot -m 64 -nic user
