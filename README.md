# arc

Minimal i386 kernel with multitasking, virtual memory, filesystems, TCP/IP networking, and POSIX-like syscalls.

## Features

- **Architecture**: i386, protected mode, higher-half kernel at `0xC0000000`
- **Multitasking**: Preemptive round-robin scheduler, COW fork, ELF32 loader
- **Memory management**: Physical page allocator with refcounting, recursive page directory, `mmap`/`munmap`
- **Filesystems**: ext2 (read/write), FAT32 (read-only), virtual `/proc`, initramfs, device filesystem
- **TCP/IP**: Full lwIP stack with e1000 NIC driver, DHCP, ARP, UDP, TCP
- **Drivers**: ACPI (poweroff/reboot), ATA PIO, AHCI, e1000, PS/2 keyboard, VGA text mode, serial console, FPU/SSE, APIC, RTC, PIT, PCI
- **Shell**: Command history (16 entries), pipes, redirection, built-in `cd`/`exit`
- **Signals**: Full signal handling (sigaction, sigprocmask, sigpending, sigsuspend, SIGCHLD, SIGSTOP/SIGCONT)
- **50+ syscalls**: fork, execve, open, read, write, mmap, pipe, signal, filesystem ops, etc.

## Commands

The kernel includes a full set of userspace utilities compiled per-ELF from `user/cmd.c`:

```
pwd  getpid  clear  ls  kill  cat  date  sleep  uname  stat  mkdir  rm
echo  touch  cp  mv  ps  whoami  id  true  false  yes  basename  dirname
wc  seq  head  which  test  cmp  hexdump  hostname  ln  chmod  chown
chgrp  free  poweroff  reboot  curl  socket_test  bench
```

## Build & Run

```
make          # build kernel + commands + disk image
make run      # boot in QEMU
make clean    # clean build artifacts
```

Requires: `gcc` (i386 cross), `ld`, `grub-mkrescue`, `qemu-system-i386`.

## Project structure

| Path | Description |
|------|-------------|
| `boot/` | Bootloader (multiboot2), linker script |
| `kernel/` | Kernel main, syscalls, initramfs, FPU, OOM, kallsyms |
| `drivers/` | ACPI, ATA, AHCI, e1000, keyboard, VGA, serial, PIT, RTC, APIC, PCI, mouse, framebuffer |
| `mm/` | Physical + virtual memory manager |
| `proc/` | Process management, scheduler, signals |
| `fs/` | ext2, FAT32, procfs, devfs, ramfs, VFS layer |
| `net/` | lwIP TCP/IP stack integration, e1000 glue, sockets |
| `interrupts/` | GDT, IDT, ISR handlers |
| `lib/` | Kernel string utils, debug print, panic |
| `include/` | Kernel headers |
| `user/` | Shell, init, libc, command suite |
| `tools/` | Build scripts (mkdisk.sh, gen_kallsyms.py) |
| `lwip/` | lwIP lightweight TCP/IP stack sources |

## License

MIT
