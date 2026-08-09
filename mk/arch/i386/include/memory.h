#ifndef MEMORY_H
#define MEMORY_H

#define PAGE_SIZE          4096
#define PAGE_SHIFT         12

/* Physical memory layout */
#define KERNEL_PHYS        0x100000

#ifdef __x86_64__
#define KERNEL_BASE        0xFFFFFFFF80000000ULL
#define DIRECT_MAP_SIZE    0x4000000ULL  /* 64MB identity via 2MB huge pages */

#define HEAP_START         0xFFFFFFFF90000000ULL
#define HEAP_END           0xFFFFFFFFA0000000ULL
#define HEAP_INITIAL_PAGES 16

#define TEMP_VADDR         0xFFFFFFFFFFFFF000ULL
#else
#define KERNEL_BASE        0xC0000000

#define HEAP_START         0xD0000000
#define HEAP_END           0xE0000000
#define HEAP_INITIAL_PAGES 4

#define TEMP_VADDR         0xFFC00000
#endif

/* User space layout (i386 defaults; amd64 has its own via mk/arch/amd64/include/memory.h) */
#ifndef __x86_64__
#define USER_BASE          0x08000000
#define USER_HEAP_START    0x40000000
#define USER_MMAP_START    0x50000000
#define USER_STACK_TOP     0xC0000000
#define USER_STACK_PAGES   32
#define USER_TLS_VADDR     0xBFFFB000
#endif

#endif
