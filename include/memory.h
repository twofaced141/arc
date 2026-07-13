#ifndef MEMORY_H
#define MEMORY_H

#define KERNEL_BASE     0xC0000000
#define KERNEL_PHYS     0x00100000

#define HEAP_START      0xD0000000
#define HEAP_END        0xFFBFFFFF
#define HEAP_INITIAL_PAGES 16

#define USER_SPACE_START 0x00400000
#define USER_SPACE_END   0xC0000000

#define USER_MMAP_START 0x20000000

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_PHYS      0x200
#define MAP_FAILED    ((void*)-1)

#endif
