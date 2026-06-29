CC       = gcc
AS       = as
LD       = ld
CFLAGS   = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
           -m32 -Wall -Wextra -std=c17 \
           -fno-pic -no-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -O0 \
           -fno-builtin -I include
ASFLAGS  = --32
LDFLAGS  = -T boot/linker.ld -nostdlib -m elf_i386

KERNEL_DIRS = kernel interrupts drivers mm proc fs lib boot
SRCS_C  = $(wildcard $(addsuffix /*.c,$(KERNEL_DIRS)))
SRCS_S  = $(wildcard $(addsuffix /*.s,$(KERNEL_DIRS)))
OBJS    = $(SRCS_C:.c=.o) $(SRCS_S:.s=.o)

all: kernel.elf user_code.elf pipe_test.elf

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.s
	$(AS) $(ASFLAGS) -o $@ $<

kernel.elf: $(OBJS) boot/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

user_code.elf: user/user_code.s
	$(AS) $(ASFLAGS) -o user_code.o user/user_code.s
	ld -Ttext=0x08000000 -nostdlib -m elf_i386 -o $@ user_code.o

pipe_test.elf: user/pipe_test.c
	$(CC) $(CFLAGS) -nostartfiles -c -o pipe_test.o user/pipe_test.c
	ld -Ttext=0x08000000 -nostdlib -m elf_i386 -o $@ pipe_test.o

clean:
	rm -rf *.o *.elf *.bin iso_root os.iso disk.img
	find . -name '*.o' -type f -delete

check: kernel.elf
	grub-file --is-x86-multiboot2 kernel.elf

TEST_FILES = user/test.txt user_code.elf pipe_test.elf

disk.img:
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	mkfs.ext2 -b 1024 -F $@ 2>/dev/null
	printf "HELLO EXT2 WORLD." > /tmp/kernel_hello.txt
	printf "NESTED FILE" > /tmp/kernel_nested.txt
	printf "write /tmp/kernel_hello.txt hello.txt\nmkdir /subdir\nwrite /tmp/kernel_nested.txt /subdir/nested.txt\n" | debugfs -w $@ 2>/dev/null
	rm -f /tmp/kernel_hello.txt /tmp/kernel_nested.txt

run: kernel.elf $(TEST_FILES) disk.img
	mkdir -p iso_root/boot/grub
	cp kernel.elf iso_root/boot/
	cp user/test.txt user_code.elf pipe_test.elf iso_root/boot/
	@echo 'set timeout=0' > iso_root/boot/grub/grub.cfg
	@echo 'set default=0' >> iso_root/boot/grub/grub.cfg
	@echo '' >> iso_root/boot/grub/grub.cfg
	@echo 'menuentry "my_kernel" {' >> iso_root/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kernel.elf' >> iso_root/boot/grub/grub.cfg
	@echo '    module2 /boot/test.txt test.txt' >> iso_root/boot/grub/grub.cfg
	@echo '    module2 /boot/user_code.elf user_code.elf' >> iso_root/boot/grub/grub.cfg
	@echo '    module2 /boot/pipe_test.elf pipe_test.elf' >> iso_root/boot/grub/grub.cfg
	@echo '    boot' >> iso_root/boot/grub/grub.cfg
	@echo '}' >> iso_root/boot/grub/grub.cfg
	grub-mkrescue -o os.iso iso_root
	qemu-system-i386 -cdrom os.iso -hda disk.img -serial stdio -no-reboot -m 64

.PHONY: all clean run check
