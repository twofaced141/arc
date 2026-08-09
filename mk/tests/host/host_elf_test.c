#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

static int failures = 0;
static int total = 0;

#define TEST(name, cond) do { \
    total++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", name); \
        failures++; \
    } else { \
        printf("  PASS: %s\n", name); \
    } \
} while(0)

#include "elf64.h"

void host_elf_tests(void) {
    printf("[elf]\n");

    TEST("ELFMAG0 == 0x7F", ELFMAG0 == 0x7F);
    TEST("ELFMAG1 == 'E'",  ELFMAG1 == 'E');
    TEST("ELFMAG2 == 'L'",  ELFMAG2 == 'L');
    TEST("ELFMAG3 == 'F'",  ELFMAG3 == 'F');
    TEST("ELF_MAGIC == 0x464C457F", ELF_MAGIC == 0x464C457F);
    TEST("ELFCLASS32 == 1", ELFCLASS32 == 1);
    TEST("ELFCLASS64 == 2", ELFCLASS64 == 2);
    TEST("ELFDATA2LSB == 1", ELFDATA2LSB == 1);
    TEST("ET_NONE == 0", ET_NONE == 0);
    TEST("ET_REL == 1",  ET_REL == 1);
    TEST("ET_EXEC == 2", ET_EXEC == 2);
    TEST("ET_DYN == 3",  ET_DYN == 3);
    TEST("ET_CORE == 4", ET_CORE == 4);
    TEST("EM_386 == 3",    EM_386 == 3);
    TEST("EM_X86_64 == 62", EM_X86_64 == 62);
    TEST("EV_CURRENT == 1", EV_CURRENT == 1);

    TEST("PT_NULL == 0",   PT_NULL == 0);
    TEST("PT_LOAD == 1",   PT_LOAD == 1);
    TEST("PT_DYNAMIC == 2", PT_DYNAMIC == 2);
    TEST("PT_INTERP == 3",  PT_INTERP == 3);
    TEST("PT_PHDR == 6",    PT_PHDR == 6);

    TEST("SHT_NULL == 0",     SHT_NULL == 0);
    TEST("SHT_PROGBITS == 1", SHT_PROGBITS == 1);
    TEST("SHT_SYMTAB == 2",   SHT_SYMTAB == 2);
    TEST("SHT_STRTAB == 3",   SHT_STRTAB == 3);

    TEST("PF_X == 1", PF_X == 1);
    TEST("PF_W == 2", PF_W == 2);
    TEST("PF_R == 4", PF_R == 4);

    TEST("STB_LOCAL == 0",  STB_LOCAL == 0);
    TEST("STB_GLOBAL == 1", STB_GLOBAL == 1);
    TEST("STB_WEAK == 2",   STB_WEAK == 2);

    TEST("STT_NOTYPE == 0", STT_NOTYPE == 0);
    TEST("STT_OBJECT == 1", STT_OBJECT == 1);
    TEST("STT_FUNC == 2",   STT_FUNC == 2);

    TEST("Elf64_Ehdr size == 64", sizeof(Elf64_Ehdr) == 64);
    TEST("Elf64_Phdr size == 56", sizeof(Elf64_Phdr) == 56);
    TEST("Elf64_Shdr size == 64", sizeof(Elf64_Shdr) == 64);
    TEST("Elf64_Sym  size == 24", sizeof(Elf64_Sym) == 24);
    TEST("Elf64_Rel  size == 16", sizeof(Elf64_Rel) == 16);
    TEST("Elf64_Rela size == 24", sizeof(Elf64_Rela) == 24);
    TEST("Elf64_Dyn  size == 16", sizeof(Elf64_Dyn) == 16);
    TEST("Elf64_Nhdr size == 12", sizeof(Elf64_Nhdr) == 12);
    TEST("auxv_t     size == 16", sizeof(auxv_t) == 16);

    {
        Elf64_Ehdr ehdr;
        TEST("e_ident offset == 0",
             (uintptr_t)&ehdr.e_ident - (uintptr_t)&ehdr == 0);
        TEST("e_type offset == 16",
             (uintptr_t)&ehdr.e_type - (uintptr_t)&ehdr == 16);
        TEST("e_entry offset == 24",
             (uintptr_t)&ehdr.e_entry - (uintptr_t)&ehdr == 24);
        TEST("e_phoff offset == 32",
             (uintptr_t)&ehdr.e_phoff - (uintptr_t)&ehdr == 32);
        TEST("e_shoff offset == 40",
             (uintptr_t)&ehdr.e_shoff - (uintptr_t)&ehdr == 40);
        TEST("e_flags offset == 48",
             (uintptr_t)&ehdr.e_flags - (uintptr_t)&ehdr == 48);
        TEST("e_ehsize offset == 52",
             (uintptr_t)&ehdr.e_ehsize - (uintptr_t)&ehdr == 52);
        TEST("e_phentsize offset == 54",
             (uintptr_t)&ehdr.e_phentsize - (uintptr_t)&ehdr == 54);
        TEST("e_phnum offset == 56",
             (uintptr_t)&ehdr.e_phnum - (uintptr_t)&ehdr == 56);
        TEST("e_shentsize offset == 58",
             (uintptr_t)&ehdr.e_shentsize - (uintptr_t)&ehdr == 58);
        TEST("e_shnum offset == 60",
             (uintptr_t)&ehdr.e_shnum - (uintptr_t)&ehdr == 60);
        TEST("e_shstrndx offset == 62",
             (uintptr_t)&ehdr.e_shstrndx - (uintptr_t)&ehdr == 62);
    }

    {
        Elf64_Phdr phdr;
        TEST("Phdr p_type offset == 0",
             (uintptr_t)&phdr.p_type - (uintptr_t)&phdr == 0);
        TEST("Phdr p_vaddr offset == 16",
             (uintptr_t)&phdr.p_vaddr - (uintptr_t)&phdr == 16);
        TEST("Phdr p_filesz offset == 32",
             (uintptr_t)&phdr.p_filesz - (uintptr_t)&phdr == 32);
    }

    {
        Elf64_Sym sym;
        TEST("Sym st_name offset == 0",
             (uintptr_t)&sym.st_name - (uintptr_t)&sym == 0);
        TEST("Sym st_value offset == 8",
             (uintptr_t)&sym.st_value - (uintptr_t)&sym == 8);
        TEST("Sym st_size offset == 16",
             (uintptr_t)&sym.st_size - (uintptr_t)&sym == 16);
    }

    TEST("ELF64_ST_BIND(0x12) == 1", ELF64_ST_BIND(0x12) == 1);
    TEST("ELF64_ST_TYPE(0x12) == 2", ELF64_ST_TYPE(0x12) == 2);
    TEST("ELF64_ST_INFO(1,2) == 0x12", ELF64_ST_INFO(1,2) == 0x12);

    printf("[elf] %d/%d passed\n", total - failures, total);
}
