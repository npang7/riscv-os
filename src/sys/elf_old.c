// elf.c - ELF file loader //
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "error.h"
#include "heap.h"
#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff; 
    uint32_t e_flags; 
    uint16_t e_ehsize; 
    uint16_t e_phentsize; 
    uint16_t e_phnum; 
    uint16_t e_shentsize; 
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

enum elf_pt {
	PT_NULL = 0, 
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR,
	PT_TLS
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define  EM_RISCV   243



/**
 * elf_load
 *
 * Description:
 *     This function reads and validates an ELF64 file, loads all PT_LOAD
 *     segments into memory at their specified virtual addresses, clear
 *     uninitialized BSS regions, and returns the entry point of the program.
 *
 * Parameters:
 *     @elfio: Pointer to an I/O interface  
 *     @eptr:  Output pointer to receive the program entry function pointer
 *
 * Returns:
 *     0 if the ELF is successfully loaded;
 *     -EINVAL if invalid headers or read errors
 *     -ENOTSUP if the ELF format is unsupported 
 *
 * Side Effects:
 *     May write data into memory at the virtual addresses
 */
// CP2 version //change

int elf_load(struct io * elfio, void (**eptr)(void)) {
    struct elf64_ehdr ehdr;

    if (ioreadat(elfio, 0, &ehdr, sizeof(ehdr)) != sizeof(ehdr))
        return -EINVAL;

    if (memcmp(ehdr.e_ident, "\x7F""ELF", 4) != 0)
        return -EINVAL;

    // Validate ELF format
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) return -ENOTSUP;   // Only support 64-bit ELF
    if (ehdr.e_ident[EI_DATA]  != ELFDATA2LSB) return -ENOTSUP;  // Only support little-endian
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) return -EINVAL; // Must be current ELF version

    if (ehdr.e_type != ET_EXEC) return -EINVAL;      // Must be executable file
    if (ehdr.e_machine != EM_RISCV) return -ENOTSUP; // Must be RISC-V architecture
    if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0 || ehdr.e_phentsize != sizeof(struct elf64_phdr)) {
        return -EINVAL; // Invalid program header table
    }
     // Load all program headers
    for (int i = 0; i < ehdr.e_phnum; i++) {
        struct elf64_phdr phdr;
        uint64_t offset = ehdr.e_phoff + i * ehdr.e_phentsize;

        if (ioreadat(elfio, offset, &phdr, sizeof(phdr)) != sizeof(phdr))
            return -EINVAL;

        if (phdr.p_type != PT_LOAD)
            continue;

        if (phdr.p_filesz > phdr.p_memsz)
            return -EINVAL;
        // valid user space
        if (phdr.p_vaddr < UMEM_START_VMA || (phdr.p_vaddr + phdr.p_memsz) > UMEM_END_VMA)
            return -EINVAL;

        //edge align
        uintptr_t start = ROUND_DOWN(phdr.p_vaddr, PAGE_SIZE);
        uintptr_t end   = ROUND_UP(phdr.p_vaddr + phdr.p_memsz, PAGE_SIZE);
        //alloc each page
        for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
            void *pa = alloc_phys_page();
            if (!pa)
                return -ENOMEM;
            if (!map_page(va, pa, PTE_R | PTE_W | PTE_X | PTE_U))
                return -EINVAL;
        }

        // segment load to pp
        if (ioreadat(elfio, phdr.p_offset, (void*)phdr.p_vaddr, phdr.p_filesz) != phdr.p_filesz)
            return -EINVAL;

        // clear bss
        if (phdr.p_memsz > phdr.p_filesz)
            memset((void*)(phdr.p_vaddr + phdr.p_filesz), 0, phdr.p_memsz - phdr.p_filesz);
    }

    *eptr = (void (*)(void))ehdr.e_entry;
    return 0;
}
