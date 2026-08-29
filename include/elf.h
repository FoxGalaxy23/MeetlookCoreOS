#ifndef ELF_H
#define ELF_H

#include "types.h"

#define EI_NIDENT 16

#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define EI_OSABI       7
#define EI_ABIVERSION  8

#define ELFMAG0        0x7f
#define ELFMAG1        'E'
#define ELFMAG2        'L'
#define ELFMAG3        'F'

#define ELFCLASS64     2
#define ELFDATA2LSB    1

#define ET_EXEC        2
#define ET_DYN         3

#define PT_LOAD        1
#define PT_INTERP      3

#define PF_X           1
#define PF_W           2
#define PF_R           4

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} Elf64_Phdr;

#endif
