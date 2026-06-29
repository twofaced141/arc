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
- **ELF32 loader** — `elf32.h`, `process_create_elf(file_t*)` in `process.c`: validates ELF magic/32-bit/EM_386/ET_EXEC, loads `PT_LOAD` segments, entry from `e_entry`, stack at `0xC0000000`.
- **ATA PIO driver (polling)** — IDENTIFY, 28-bit LBA PIO read.
- **Ext2 read-only driver** — superblock, block groups, inodes (dir/file read).
- **COW fork** — `SYSCALL_FORK = 14`, `process_fork()`, `vmm_fork_cow_pages()`, COW page fault handler.
- **`process_exit` cleanup** — closes FDs, frees kernel stack.
- **`current_index` reset** — `context_switch` sets `current_index = -1` when no READY/RUNNING process.
- **TEMP_VADDR PDE pre-created** — in `vmm_init` for COW page fault handler.
- **ext2 integration with fd system** — `ext2_ino` field in `file_t`, ext2 fallback in `fs_open`, `ext2_read_file` support in `fs_read`.
- **execve syscall** — `SYSCALL_EXECVE = 15`, `process_exec()`: reads ELF via `fs_open`, validates, clears user pages via `vmm_clear_user_pages` (skips kernel-shared PDEs), loads new segments, sets up fresh register frame and stack. Tested with `user_code.elf → execve("user_code2.elf")`.
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
- **COW fork** over eager full copy.
- **Ext2 integrated into `fs_open`** as fallback — initrd checked first, then ext2.
- **`vmm_clear_user_pages`** frees only process-private user PDEs; kernel-shared PDEs (identity map, higher half) are preserved.
- **execve modifies `r` directly** — doesn't create new frame; `scheduler_switch` saves/returns `r`, so `iret` uses the modified eip/useresp.
- **Struct assignment avoided for `registers_t`** — explicit word-by-word copy loop for `-fno-builtin` compatibility.

## Next Steps
1. **Page reference counting** — free COW physical pages only when last process releases them
2. **Argument parsing** — shell currently passes the whole input as one arg to execve; split by spaces
3. **Serial console input** — pipe `-serial stdio` input to stdin for keyboard-less operation
4. **Cross-compiled TinyCC** as initrd module for on-the-fly C compilation (optional)

## Relevant Files (new structure)
- `proc/process.c` — `process_create_elf`, `process_fork`, `process_exec`, `process_exit`
- `include/process.h` — declarations, `PROC_UNUSED(0)`, `READY(1)`, `RUNNING(2)`, `BLOCKED(3)`, `ZOMBIE(4)`, `cwd_inode` field
- `mm/vmm.c` — `vmm_fork_cow_pages`, `vmm_clear_user_pages`, COW handler in `page_fault_handler`
- `include/vmm.h` — `VMM_COW = (1 << 9)`, `TEMP_VADDR = 0xFFC00000`
- `include/isr.h` — `SYSCALL_FORK = 14u`, `SYSCALL_EXECVE = 15u`, `SYSCALL_CHDIR = 17u`, `SYSCALL_GETCWD = 18u`, `SYSCALL_LISTDIR = 19u`
- `kernel/kernel.c` — syscall_handler with cases 0-19, ext2 registration
- `proc/scheduler.c` — `context_switch` with `current_index = -1` fallback
- `include/fs.h`, `fs/fs.c` — `file_t` with `ext2_ino`, `fs_open` ext2 fallback, `fs_get_ext2()`
- `include/ext2.h`, `fs/ext2.c` — read-only ext2 driver, `ext2_resolve()`, `ext2_find_name()`, `ext2_read_names()`
- `user/user_code.s` — shell with built-ins: `exit`, `pwd`, `cd`, `ls`, fork+execve for everything else
