# arc

Minimal i386 kernel with multitasking, virtual memory, filesystems, TCP/IP networking, POSIX-like syscalls, and userspace sockets.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-i386-blue.svg)]()
[![Build](https://img.shields.io/badge/build-make-green.svg)]()

## Features

- **Architecture**: i386, protected mode, higher-half kernel at `0xC0000000`
- **Multitasking**: Preemptive round-robin scheduler, COW fork, ELF32 loader, nice priorities
- **Memory management**: Physical page allocator with refcounting, recursive page directory, mmap/munmap (anonymous, file-backed, physical), brk/sbrk heap
- **Filesystems**: ext2 (read/write with symlinks, hard links, mkdir, rmdir, chmod, chown, truncate), FAT32 (read-only), virtual `/proc`, initramfs, devfs (`/dev/null`, `/dev/zero`, `/dev/random`, block devices)
- **TCP/IP**: Full lwIP stack with e1000 NIC driver, DHCP, ARP, UDP, TCP — exposed to userspace via socket/bind/connect/listen/accept/send/recv syscalls
- **Drivers**: ACPI (poweroff/reboot), ATA PIO, AHCI (SATA), e1000, PS/2 keyboard + mouse, VGA text mode, framebuffer console, serial console, FPU/SSE, APIC (I/O APIC + local APIC), RTC, PIT, PCI
- **Shell**: Command history (16 entries), pipes, output redirection, built-in `cd`/`exit`
- **Signals**: 31 signals, sigaction, sigreturn, sigprocmask, sigpending, sigsuspend, SIGCHLD, SIGSTOP/SIGCONT, SIGALRM
- **91 syscalls**: fork, execve, open/read/write, mmap, pipe, socket/bind/connect/listen/accept/send/recv, mount, signals, filesystem ops, credentials, scheduling, framebuffer, mouse, OOM control
- **Linux GLIBC compat layer**: Translates Linux i386 syscall numbers to native, for running GLIBC-compiled binaries

## Commands (42)

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
| `kernel/` | Main, syscalls, initramfs, FPU, OOM, kallsyms |
| `drivers/` | ACPI, ATA, AHCI, e1000, keyboard, mouse, VGA, framebuffer, serial, PIT, RTC, IOAPIC, LAPIC, PCI |
| `mm/` | Physical + virtual memory manager |
| `proc/` | Process management, scheduler, signals |
| `fs/` | ext2, FAT32, procfs, devfs, ramfs, VFS layer |
| `net/` | lwIP TCP/IP stack integration, socket layer, e1000 glue |
| `interrupts/` | GDT, IDT, ISR handlers |
| `lib/` | Kernel string utils, debug print, panic |
| `include/` | Kernel headers |
| `user/` | Shell, init, libc, command suite (42 cmds) |
| `tools/` | Build scripts |
| `lwip/` | lwIP lightweight TCP/IP stack sources |

## License

MIT
