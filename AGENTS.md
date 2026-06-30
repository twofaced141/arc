# Summary

## Goal
Build a minimal i386 kernel with multitasking, syscalls, PCI/ATA/ext2, ELF32 loading, COW fork, ext2-backed fd system, execve, user-space ELF execution, virtual /proc filesystem, shell with command history, and a full set of /bin commands.

## Constraints & Preferences
- All source in `~/Projects/kernel/`
- Source layout: `boot/` `kernel/` `interrupts/` `drivers/` `mm/` `proc/` `fs/` `lib/` `include/` `user/`
- Makefile auto-discovers `.c`/`.s` via `KERNEL_DIRS` list; add new dirs by editing it
- Headers in `include/`; `-I include` added to CFLAGS, so `#include "foo.h"` works from any subdir
- Freestanding i386 kernel, no libc, `-fno-builtin` to prevent implicit `memcpy` calls
- Higher-half mapped at `0xC0000000`, heap at `0xD0000000`–`0xE0000000`
- GRUB-multiboot2 boot via ISO image; QEMU with `-hda disk.img` for ATA/ext2
- Debug over COM1 serial; `debug_printf` supports `%x`, `%p`, `%s`, `%u`, `%d` only — no width specifiers (`%02x`), no `%llu`
- `make run` passes test files as multiboot2 modules
- `disk.img` formatted as ext2 (1024-byte blocks) for filesystem testing
- No `ksnprintf` in kernel — proc content built with manual `append()`/`append_dec()` helpers
- User commands compiled per-ELF with `-DCMD_<name>` from `user/cmd.c`

## Progress
### Done
- **`/bin/init` boot** — `kernel_main` loads `/bin/init` from ext2 after ATA/ext2 init. Init (PID 1) forks and execves `/bin/shell`, respawns on exit.
- **17 `/bin/<cmd>` commands** — all in `user/cmd.c` compiled per-ELF: pwd, getpid, clear, ls, kill, cat, date, sleep, uname, stat, mkdir, rm, echo, touch, cp, mv, ps
- **Shell with command history** — `user/shell.c`: `cd`/`exit` built-ins, fork+execve for rest, history ring buffer (16 entries), up/down arrow navigation.
- **execve args** — `process_exec` accepts `const char *args`, copies to `0xBFFFF000` on user stack; `r->ecx = 0xBFFFF000` for user-space access.
- **Stack trace in panic** — `panic_stack_trace` walks the x86 EBP frame-pointer chain (up to 16 frames), prints return addresses; skips user-mode.
- **Separate disk image** — `tools/mkdisk.sh` creates `disk.img` via `debugfs`; `make mkdisk` rebuilds.
- **ELF32 loader** — `elf32.h`, `process_create_elf(file_t*)` in `process.c`: validates ELF magic/32-bit/EM_386/ET_EXEC, loads `PT_LOAD` segments, entry from `e_entry`, stack at `0xC0000000`.
- **ATA PIO driver (polling)** — IDENTIFY, 28-bit LBA PIO read.
- **Ext2 read-write driver** — superblock, block groups, inodes (dir/file read/write), unlink, rename, mkdir, rmdir, truncate, create, add_dirent, free_inode, free_block, stat, getdents.
- **COW fork** — `SYSCALL_FORK = 14`, `process_fork()`, `vmm_fork_cow_pages()`, COW page fault handler with refcounting.
- **Page reference counting** — `pmm_refcount_init/inc/dec` (`mm/pmm.c`); used in COW fork, `vmm_free_directory`, `vmm_clear_user_pages`, COW-handler, munmap. Physical page freed only when last reference dropped.
- **`process_exit` cleanup** — closes FDs, frees kernel stack, calls `vmm_free_directory(proc->page_dir)` (line 560).
- **`current_index` reset** — `context_switch` sets `current_index = -1` when no READY/RUNNING process.
- **uid/gid fields in `process_t`** — `uid`, `gid`, `euid`, `egid` for POSIX credential syscalls
- **Virtual /proc filesystem** — `fs/procfs.c`, `include/procfs.h`; files: cpuinfo, meminfo, uptime, version, stat, `/proc/<pid>/status`, `/proc/<pid>/mem`. FD_PROC type (9) in `fd.h`. Injected into root directory listings.
- **Keyboard arrow keys → escape sequences** — `drivers/keyboard.c`: detects E0 prefix, pushes `\x1B[A`/`\x1B[B`/`\x1B[C`/`\x1B[D` for up/down/right/left.
- **VGA `\r` support** — `drivers/vga.c`: `terminal_putchar` handles `\r` (carriage return) by resetting column to 0.
- **libc** — `user/libc/`: `string.o`, `stdio.o` (printf/sprintf), `stdlib.o` (malloc/atoi), `ctype.o`, `errno.o`, `unistd.o` (syscall wrappers). Headers: `dirent.h`, `fcntl.h`, `sys/stat.h`.
- **All syscalls implemented** (0–44):
  | # | Name | # | Name |
  |---|------|---|------|
  | 0 | getpid | 22 | pipe |
  | 1 | putc | 23 | ioctl |
  | 2 | yield | 24 | gettime |
  | 3 | exit | 25 | kmalloc_test |
  | 4 | write | 26 | uname |
  | 5 | read | 27 | mmap |
  | 6 | sleep | 28 | munmap |
  | 7 | getticks | 29 | stat |
  | 8 | open | 30 | lstat |
  | 9 | close | 31 | getdents |
  | 10 | lseek | 32 | unlink |
  | 11 | fstat | 33 | rename |
  | 12 | brk | 34 | mkdir |
  | 13 | sbrk | 35 | rmdir |
  | 14 | fork (COW) | 36 | chmod |
  | 15 | execve | 37 | chown |
  | 16 | waitpid | 38 | access |
  | 17 | chdir | 39 | getuid |
  | 18 | getcwd | 40 | getgid |
  | 19 | listdir | 41 | geteuid |
  | 20 | kill | 42 | getegid |
  | 21 | dup2 | 44 | sigaction/sigreturn |

### Fixed
- **vmm_fork_cow_pages** now skips PDEs matching `kernel_directory->entries[]` (identity-mapped PDEs 0-15). Previously it allocated private copies of kernel page tables for the child, causing `vmm_clear_user_pages` (in execve) to free them since they didn't match the kernel's PDEs.
- **Shell process count reduced to 1** — was 2, causing keyboard input to be split between shells.
- **All debug_printf lifecycle logs removed** — only allocator (`pmm_alloc`) logs remain.
- **Deleted dirents no longer terminate directory scans** — all 7 `for`/`while` loops in `ext2.c` that scan directory entries used `break` on `de->inode == 0` (a deleted entry), causing every entry after a deletion to be invisible. Changed to `pos += de->rec_len; continue;`.
- **Duplicate name detection in ext2** — `ext2_add_dirent` now checks `ext2_lookup` before inserting; `ext2_rename` pre-checks destination to avoid leaving the source unlinked on failure.
- **User stack increased from 1 to 4 pages** — commands with deeper call stacks caused SIGSEGV at `BFFFxxxx`. `USER_STACK_PAGES = 4` in `process.h`.
- **ps: stack overflow** — `dirent dirs[64]` (~17 KB) exceeded 16 KB user stack. Reduced to 16.
- **ps: PID field parsing** — spurious `line[4] == ':'` check (format is `Pid:   `, not `Pid::`). Removed.
- **VGA: `\r` carriage return** — `terminal_putchar` printed `\r` as literal character. Now sets `terminal_column = 0`.

### Known Issues
- `/proc/<pid>/` subdir traversal (`fd_open` routing for `/proc/<pid>/status` path) works for leaf files; directory listing within `/proc/<pid>/` returns entries but deeper nesting not tested.

## Key Decisions
- **`/bin/init` from ext2** — kernel no longer hardcodes shell ELF from initrd; ext2 must be functional for userspace to start.
- **COW fork** over eager full copy, backed by page reference counting in `pmm.c`.
- **`vmm_clear_user_pages`** frees only process-private user PDEs; kernel-shared PDEs (identity map, higher half) are preserved.
- **execve modifies `r` directly** — doesn't create new frame; `scheduler_switch` saves/returns `r`, so `iret` uses the modified eip/useresp.
- **Struct assignment avoided for `registers_t`** — explicit word-by-word copy loop for `-fno-builtin` compatibility.
- **/proc is virtual, generated on open** — no on-disk storage; manual string helpers avoid `ksnprintf` dependency.
- **Keyboard arrows produce escape sequences** — PS/2 extended scancodes (E0-prefix) converted to `\x1B[A` etc. by keyboard IRQ handler for terminal-style input.

## Next Steps
1. **ATA PIO write + ext2 block allocator** — enable `ext2_write` to allocate new blocks and extend files (needed for `echo > file`, file editing).
2. **Serial console input** — pipe COM1 input to stdin (fd 0) for keyboard-less operation via `-serial stdio`.
3. **mmap for ext2 files** — map file contents into process address space.

## Relevant Files
- `user/shell.c` — shell with command history (16 entries), up/down arrow navigation
- `user/cmd.c` — all 17 utility commands compiled per-ELF with `-DCMD_<name>`
- `user/init.c` — PID 1: fork+execve `/bin/shell`, waitpid, respawn
- `user/libc/` — minimal libc: `string.c`, `stdio.c`, `stdlib.c`, `ctype.c`, `errno.c`, `unistd.c`, `dirent.h`, `fcntl.h`, `sys/stat.h`
- `proc/process.c` — `process_create_elf`, `process_fork` (COW), `process_exec` (args at `0xBFFFF000`), `process_exit` (calls `vmm_free_directory`)
- `include/process.h` — `PROC_KSTACK_SIZE 8192`, `USER_HEAP_START 0x40000000`, `USER_STACK_PAGES 4`, uid/gid fields
- `mm/vmm.c` — `vmm_fork_cow_pages`, `vmm_clear_user_pages`, COW handler with refcount calls
- `include/vmm.h` — `VMM_COW = (1 << 9)`, `TEMP_VADDR = 0xFFC00000`
- `mm/pmm.c` — page reference counting: `pmm_refcount_init/inc/dec`, frees page when refcount hits 0
- `drivers/keyboard.c` — PS/2 keyboard IRQ handler with E0-prefix detection, arrow → escape sequence translation
- `drivers/vga.c` — VGA text-mode terminal with `\r`, `\b`, `\n` support
- `include/isr.h` — syscall numbers 0–44
- `kernel/syscalls.c` — all 44 syscall handlers; getdents/listdir inject "proc" into root; stat/access route /proc
- `kernel/kernel.c` — `kernel_main`, `pmm_refcount_init()` call, ext2 registration
- `proc/scheduler.c` — `context_switch` with `current_index = -1` fallback
- `include/ext2.h`, `fs/ext2.c` — ext2 read-write driver with unlink, rename, mkdir, rmdir, truncate, create, add_dirent, free_inode, free_block, stat, getdents
- `include/procfs.h`, `fs/procfs.c` — virtual /proc filesystem
- `include/fd.h`, `fs/fd.c` — `FD_PROC = 9`, fd_open routes /proc paths, fd_lseek/fd_fstat support FD_PROC
- `tools/mkdisk.sh` — builds `disk.img` with all 17 `/bin/<cmd>` ELFs
- `Makefile` — `CMD_NAMES` list, `cmd_%.elf` rule, `mkdisk` depends on `$(CMD_ELFS)`
