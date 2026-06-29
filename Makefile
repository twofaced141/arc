CC       = gcc
AS       = as
LD       = ld
CFLAGS   = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
           -m32 -Wall -Wextra -std=c17 \
           -fno-pic -no-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -O0
ASFLAGS  = --32
LDFLAGS  = -T linker.ld -nostdlib -m elf_i386

OBJS = boot.o gdt.o kernel.o interrupts.o idt.o isr.o keyboard.o pit.o debug.o panic.o pmm.o vmm.o

all: kernel.elf

boot.o: boot.s
	$(AS) $(ASFLAGS) -o $@ $<

gdt.o: gdt.c gdt.h
	$(CC) $(CFLAGS) -c -o $@ $<

interrupts.o: interrupts.s
	$(AS) $(ASFLAGS) -o $@ $<

idt.o: idt.c idt.h
	$(CC) $(CFLAGS) -c -o $@ $<

isr.o: isr.c isr.h idt.h
	$(CC) $(CFLAGS) -c -o $@ $<

keyboard.o: keyboard.c keyboard.h isr.h idt.h terminal.h
	$(CC) $(CFLAGS) -c -o $@ $<

pit.o: pit.c pit.h idt.h isr.h
	$(CC) $(CFLAGS) -c -o $@ $<

debug.o: debug.c debug.h idt.h
	$(CC) $(CFLAGS) -c -o $@ $<

panic.o: panic.c panic.h isr.h debug.h
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.o: kernel.c terminal.h idt.h isr.h keyboard.h debug.h pmm.h vmm.h multiboot2.h
	$(CC) $(CFLAGS) -c -o $@ $<

pmm.o: pmm.c pmm.h multiboot2.h terminal.h
	$(CC) $(CFLAGS) -c -o $@ $<

vmm.o: vmm.c vmm.h pmm.h terminal.h debug.h panic.h isr.h
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -rf *.o *.elf iso_root os.iso

# Новая цель для проверки валидности Multiboot2 заголовка перед запуском
check: kernel.elf
	grub-file --is-x86-multiboot2 kernel.elf

# Создание ISO и запуск через CD-ROM
run: kernel.elf
	mkdir -p iso_root/boot/grub
	cp kernel.elf iso_root/boot/
	@echo 'set timeout=0' > iso_root/boot/grub/grub.cfg
	@echo 'set default=0' >> iso_root/boot/grub/grub.cfg
	@echo '' >> iso_root/boot/grub/grub.cfg
	@echo 'menuentry "my_kernel" {' >> iso_root/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/kernel.elf' >> iso_root/boot/grub/grub.cfg
	@echo '    boot' >> iso_root/boot/grub/grub.cfg
	@echo '}' >> iso_root/boot/grub/grub.cfg
	grub-mkrescue -o os.iso iso_root
	qemu-system-i386 -cdrom os.iso -serial stdio -no-reboot

.PHONY: all clean run check
