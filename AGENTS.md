# Summary

## Goal
Build a minimal i386 kernel with multitasking, syscalls, PCI/ATA/ext2, ELF32 loading, COW fork, ext2-backed fd system, execve, and user-space ELF execution.

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

## Progress
### Done
- **`/bin/init` boot** — `kernel_main` loads `/bin/init` from ext2 after ATA/ext2 init. Init (PID 1) forks and execves `/bin/shell`, respawns on exit.
- **Modular `/bin/<cmd>` commands** — `user/cmd.c` compiled per-command (pwd, getpid, clear, ls, kill, cat); `user/shell.c` is minimal prompt (cd/exit built-ins only, fork+execve `/bin/<cmd>` for rest).
- **execve args** — `process_exec` accepts `const char *args`, copies to `0xBFFFF000` on user stack; `r->ecx = 0xBFFFF000` for user-space access. `SYSCALL_EXECVE` passes `r->ecx` as args pointer.
- **Stack trace in panic** — `panic_stack_trace` walks the x86 EBP frame-pointer chain (up to 16 frames), prints return addresses; skips user-mode.
- **Separate disk image** — `tools/mkdisk.sh` creates `disk.img`; `make mkdisk` to build, `make run` just uses the pre-existing image.
- **ELF32 loader** — `elf32.h`, `process_create_elf(file_t*)` in `process.c`: validates ELF magic/32-bit/EM_386/ET_EXEC, loads `PT_LOAD` segments, entry from `e_entry`, stack at `0xC0000000`.
- **ATA PIO driver (polling)** — IDENTIFY, 28-bit LBA PIO read.
- **Ext2 read-only driver** — superblock, block groups, inodes (dir/file read).
- **COW fork** — `SYSCALL_FORK = 14`, `process_fork()`, `vmm_fork_cow_pages()`, COW page fault handler.
- **`process_exit` cleanup** — closes FDs, frees kernel stack.
- **`current_index` reset** — `context_switch` sets `current_index = -1` when no READY/RUNNING process.
- **TEMP_VADDR PDE pre-created** — in `vmm_init` for COW page fault handler.
- **ext2 integration with fd system** — `ext2_ino` field in `file_t`, ext2 fallback in `fs_open`, `ext2_read_file` support in `fs_read`.
- **execve syscall** — `SYSCALL_EXECVE = 15`, `process_exec()`: reads ELF via `fs_open`, validates, clears user pages via `vmm_clear_user_pages` (skips kernel-shared PDEs), loads new segments, sets up fresh register frame and stack.
- **All syscalls implemented**:
  | # | Name |
  |---|------|
  | 0 | getpid |
  | 1 | putc |
  | 2 | yield |
  | 3 | exit |
  | 4 | write |
  | 5 | read |
  | 6 | sleep |
  | 7 | getticks |
  | 8 | open |
  | 9 | close |
  | 10 | lseek |
  | 11 | fstat |
  | 12 | brk |
  | 13 | sbrk |
  | 14 | fork (COW) |
  | 15 | execve |
  | 16 | waitpid |
  | 17 | chdir |
  | 18 | getcwd |
  | 19 | listdir |

### Fixed
- **vmm_fork_cow_pages** now skips PDEs matching `kernel_directory->entries[]` (identity-mapped PDEs 0-15). Previously it allocated private copies of kernel page tables for the child, causing `vmm_clear_user_pages` (in execve) to free them since they didn't match the kernel's PDEs.
- **Shell process count reduced to 1** — was 2, causing keyboard input to be split between shells.
- **All debug_printf lifecycle logs removed** — only allocator (`pmm_alloc`) logs remain.

### Known Issues
- `process_exit` skips `vmm_free_directory` — COW-shared physical pages would be double-freed; proper refcounting needed.

## Key Decisions
- **`/bin/init` from ext2** — kernel no longer hardcodes shell ELF from initrd; ext2 must be functional for userspace to start.
- **COW fork** over eager full copy.
- **Ext2 integrated into `fs_open`** as fallback — initrd checked first, then ext2.
- **`vmm_clear_user_pages`** frees only process-private user PDEs; kernel-shared PDEs (identity map, higher half) are preserved.
- **execve modifies `r` directly** — doesn't create new frame; `scheduler_switch` saves/returns `r`, so `iret` uses the modified eip/useresp.
- **Struct assignment avoided for `registers_t`** — explicit word-by-word copy loop for `-fno-builtin` compatibility.

## Next Steps
1. **Page reference counting** — free COW physical pages only when last process releases them
2. **Serial console input** — pipe `-serial stdio` input to stdin for keyboard-less operation
3. **Cross-compiled TinyCC** as initrd module for on-the-fly C compilation (optional)
4. **`process_exit` calls `vmm_free_directory`** — blocked by COW refcounting

## Relevant Files (new structure)
- `user/shell.c` — minimal prompt: `cd`/`exit` built-in, fork+execve `/bin/<cmd>` for rest
- `user/cmd.c` — all utility commands compiled per-ELF with `-DCMD_<name>`
- `user/init.c` — PID 1: fork+execve `/bin/shell`, waitpid, respawn
- `proc/process.c` — `process_create_elf`, `process_fork`, `process_exec` (now accepts args, copies to `0xBFFFF000`), `process_exit`
- `include/process.h` — `process_exec` signature: `(proc, path, args, r, cwd_inode)`
- `mm/vmm.c` — `vmm_fork_cow_pages`, `vmm_clear_user_pages`, COW handler
- `include/vmm.h` — `VMM_COW = (1 << 9)`, `TEMP_VADDR = 0xFFC00000`
- `include/isr.h` — syscall numbers 0-23 (kill=20, dup2=21, pipe=22, ioctl=23)
- `kernel/kernel.c` — syscall_handler, ext2 registration
- `proc/scheduler.c` — `context_switch` with `current_index = -1` fallback
- `include/fs.h`, `fs/fs.c` — `file_t` with `ext2_ino`, `fs_open` ext2 fallback
- `include/ext2.h`, `fs/ext2.c` — ext2 driver
- `tools/mkdisk.sh` — builds `disk.img` with `/bin/init`, `/bin/shell`, `/bin/{pwd,getpid,clear,ls,kill,cat}`
- `Makefile` — `CMD_NAMES`, `cmd_%.elf` rule, `mkdisk` depends on `$(CMD_ELFS)`
