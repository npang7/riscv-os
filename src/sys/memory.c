/*! @file memory.c
    @brief Physical and virtual memory manager    
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "error.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size: 512 × 4KB = 2MB
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size: 512 × 2MB = 1GB

#define PTE_ORDER 3   //PTE: 2^3 = 8byte
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))  // 512

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk * next; ///< Next page in list
    unsigned long pagecnt; ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2*9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1*9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0*9)) % PTE_CNT)

#define MIN(a,b) (((a)<(b))?(a):(b))

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k)) 
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) \
                             >> (lvl * (PAGE_ORDER - PTE_ORDER)))
#define is_in_user(vma) (UMEM_START_VMA <= (vma) && (vma) < UMEM_END_VMA)

// INTERNAL FUNCTION DECLARATIONS
//



static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte * root, unsigned int asid);
static inline struct pte * mtag_to_ptab(mtag_t mtag);
static inline struct pte * active_space_ptab(void);

static inline void * pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void * p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
// 


/**
 * @brief Initialize memory management and page tables.
 *
 * Sets up kernel page tables, heap allocator, and free page chunk list.
 * Enables paging and allows access to user memory.
 * @arg None
 * @note Must be called once during start.
 * @return None
 * @sideeffects: Writes to the "satp" CSR to activate paging.
 */
void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    
    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void*)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);
    

    
    // TODO: Initialize the free chunk list here
    uintptr_t chunk_start = (uintptr_t)heap_end;
    uintptr_t chunk_end = (uintptr_t)RAM_END;

    // round to page edge
    chunk_start = ROUND_UP(chunk_start, PAGE_SIZE);
    chunk_end = ROUND_DOWN(chunk_end, PAGE_SIZE);

    if (chunk_start >= chunk_end) {
        panic("No memory left for free chunk list");
    }

    // calculate pages can alloc
    unsigned long total_pages = (chunk_end - chunk_start) / PAGE_SIZE;

    // set first chunk to start
    struct page_chunk * chunk = (struct page_chunk *)chunk_start;
    chunk->pagecnt = total_pages;
    chunk->next = NULL;

    // set head pointer
    free_chunk_list = chunk;
    
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) {
    return active_space_mtag();
}

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;
    
    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}



/**
 * @brief Clone the current active user address space (excluding kernel/global mappings).
 * 
 * @arg None
 * @return mtag_t: A memory tag referring to the new cloned address space; returns -1 on failure.
 * 
 * @sideeffects:
 * Modifies physical memory state and page table metadata.
 */
mtag_t clone_active_mspace(void) {
    struct pte *src_pt2 = active_space_ptab();  // get current root
    struct pte *new_pt2 = alloc_phys_page();    // alloc new root
    if (!new_pt2) return -1; //fail

    memcpy(new_pt2, src_pt2, PAGE_SIZE);  // copy all level 2 PTE

    for (int i = 0; i < PTE_CNT; ++i ) {
        struct pte *src_pte = &src_pt2[i];
        struct pte *new_pte = &new_pt2[i];

        uintptr_t vma_l2 = (uintptr_t)(i) * GIGA_SIZE;

        //check
        if (PTE_GLOBAL(*src_pte)) continue;
        if (!PTE_VALID(*src_pte)) continue;
        if (!is_in_user(vma_l2)) continue;
        ///if (PTE_LEAF(*src_pte)) continue;  // Gigapage page


        struct pte *src_pt1 = (struct pte *)((uintptr_t)(src_pte->ppn) << 12);
        struct pte *new_pt1 = alloc_phys_page();
        if (!new_pt1) {
            free_phys_page(new_pt2);
            return -1;
        }
        memset(new_pt1, 0, PAGE_SIZE); //clear
        *new_pte = ptab_pte(new_pt1, src_pte->flags);

        for (int j = 0; j < PTE_CNT; j++) {
            struct pte *src_pt1_pte = &src_pt1[j];
            struct pte *new_pt1_pte = &new_pt1[j];

            uintptr_t vma_l1 = vma_l2 + ((uintptr_t)j * MEGA_SIZE);

            //check
            // if (PTE_GLOBAL(*src_pt1_pte)) continue;
            if (!PTE_VALID(*src_pt1_pte)) continue;
            // if (!is_in_user(vma_l1)) continue;
            if (PTE_LEAF(*src_pt1_pte)) continue;   // Megapage

            struct pte *src_pt0 = (struct pte *)((uintptr_t)(src_pt1_pte->ppn) << 12);
            struct pte *new_pt0 = alloc_phys_page();
            if (!new_pt0) {
                free_phys_page(new_pt1);
                free_phys_page(new_pt2);
                return -1;
            }
            memset(new_pt0, 0, PAGE_SIZE);
            *new_pt1_pte = ptab_pte(new_pt0, src_pt1_pte->flags);

            for (int k = 0; k < PTE_CNT; k++) {
                struct pte *src_pt0_pte = &src_pt0[k];
                struct pte *new_pt0_pte = &new_pt0[k];
                uintptr_t vma_l0 = vma_l1 + ((uintptr_t)k * PAGE_SIZE);

                if (!PTE_VALID(*src_pt0_pte)) continue;
                if (!PTE_LEAF(*src_pt0_pte) || PTE_GLOBAL(*src_pt0_pte)) continue;
                if (!is_in_user(vma_l0)) continue;

                // user page, copy
                void *new_page = alloc_phys_page();
                if (!new_page) return -1;
                void *src_page = (void *)((uintptr_t)(src_pt0_pte->ppn) << 12);
                memcpy(new_page, src_page, PAGE_SIZE);
                *new_pt0_pte = leaf_pte(new_page, src_pt0_pte->flags);


            }
        }
    }
    
    mtag_t mtag = ptab_to_mtag(new_pt2, 0);
    sfence_vma(); 
    return mtag;
}



//helper func to support specific mtag
void reset_specific_mspace(struct pte *pt2) {

    for (size_t i2 = 0; i2 < PTE_CNT; i2++) {
        if (!PTE_VALID(pt2[i2]) || PTE_LEAF(pt2[i2]) || PTE_GLOBAL(pt2[i2]))
            continue;

        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[i2].ppn << 12);
        int pt1_empty = 1;
        for (size_t i1 = 0; i1 < PTE_CNT; i1++) {
            if (!PTE_VALID(pt1[i1]) || PTE_LEAF(pt1[i1]) || PTE_GLOBAL(pt1[i1]))
                continue;
            // check whether empty pte
            uintptr_t pt0_pa = (uintptr_t)pt1[i1].ppn << 12;
            if (pt0_pa == 0) continue;

            struct pte *pt0 = (struct pte *)((uintptr_t)pt1[i1].ppn << 12);
            int pt0_empty = 1;
            for (size_t i0 = 0; i0 < PTE_CNT; i0++) {
                if (!PTE_VALID(pt0[i0]) || PTE_GLOBAL(pt0[i0]))
                    continue;
                if (PTE_LEAF(pt0[i0])) {
                    uintptr_t pa = (uintptr_t)pt0[i0].ppn << 12;
                    if (pa != 0) {
                        free_phys_page((void *)pa); //free
                    }
                }
                pt0[i0] = null_pte();
            }

            // check whether pt0 all empty
            for (size_t k = 0; k < PTE_CNT; k++) {
                if (PTE_VALID(pt0[k])) {
                    pt0_empty = 0;
                    break; //exist at least one. break
                }
            }
            if (pt0_empty) { //if all-empty
                free_phys_page(pt0);
                pt1[i1] = null_pte(); //free pointer from pt1
            } else {
                pt1_empty = 0;
            }
        }

        // check whether pt1 all empty
        for (size_t k = 0; k < PTE_CNT; k++) {
            if (PTE_VALID(pt1[k])) {
                pt1_empty = 0;
                break;
            }
        }
        if (pt1_empty) {
            free_phys_page(pt1);
            pt2[i2] = null_pte(); //free pointer from pt2
        }
    }
}
/**
 * @brief Reset and reclaim all non-global page table entries in the active address space.
 * This function traverses the three-level page tables starting from pt2,
 * freeing all dynamically allocated physical pages and page tables associated
 * with the current active address space, except for global and leaf mappings.
 * @arg None
 * @return None
 * @sideeffects:
 * Clears non-global page table entries in the active space.
 * Modifies the active page table hierarchy.
 */
void reset_active_mspace(void) {
    struct pte *pt2 = active_space_ptab();  
    reset_specific_mspace(pt2);
    sfence_vma();
}



/**
 * Helper func
 * @brief Reset and reclaim all non-global page table entries in the previous address space.
 * @arg prev tag
 * @return None
 * @sideeffects:
 * Clears non-global page table entries in the active space.
 * Modifies the active page table hierarchy.
 */
void reset_prev_mspace(mtag_t prev) {
    struct pte *pt2 = mtag_to_ptab(prev); 
    reset_specific_mspace(pt2);
    sfence_vma();
}


/**
 * @brief Discard the current active memory space and switch to the main space.
 *
 * @return The satp tag of the discarded memory space.
 *
 * @sideeffects: None.
 */
mtag_t discard_active_mspace(void) {
    mtag_t prev = switch_mspace(main_mtag);                  // switch back to main address space
    reset_prev_mspace(prev);                    
    return main_mtag;                                
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

/**
 * @brief Map a physical page to a virtual address in the current address space.
 *
 * @param vma         Virtual address to map.
 * @param pp          Pointer to the physical page to be mapped.
 * @param rwxug_flags Flags for access permissions.
 *
 * @return The mapped virtual address vma.
 *
 * @sideeffects:
 * Allocates new page tables if necessary.
 * Modifies the current page table by inserting a new mapping.
 */
void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    struct pte *pt2 = active_space_ptab();  // root pt
    struct pte *pt1, *pt0;
    unsigned vpn2 = VPN2(vma);
    unsigned vpn1 = VPN1(vma);
    unsigned vpn0 = VPN0(vma);

    // level 1 pt
    if (!PTE_VALID(pt2[vpn2])) {
        pt1 = alloc_phys_page();  // allocate new page table page
        memset(pt1, 0, PAGE_SIZE);
        pt2[vpn2] = ptab_pte(pt1, 0); // store as page table entry
    } else {
        pt1 = (struct pte *)((uintptr_t)(pt2[vpn2].ppn) << 12);
    }

    // level 0 pt
    if (!PTE_VALID(pt1[vpn1])) {
        pt0 = alloc_phys_page();  
        memset(pt0, 0, PAGE_SIZE);
        pt1[vpn1] = ptab_pte(pt0, 0);
    } else {
        pt0 = (struct pte *)((uintptr_t)(pt1[vpn1].ppn) << 12);
    }
    // leaf pt entry
    pt0[vpn0] = leaf_pte(pp, rwxug_flags); 
    return (void *)vma;
}

/**
 * @brief Maps a range of physical pages to a range of virtual addresses.
 *
 * @param vma         Starting virtual address to map.
 * @param size        The size of the range to map (in bytes).
 * @param pp          Pointer to the starting physical page.
 * @param rwxug_flags Flags specifying access permissions
 * @return The starting virtual address vam.
 *
 * @sideeffects:
 * Modifies the page table entries for the given va range.
 */
void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    uintptr_t va = vma;
    uintptr_t pa = (uintptr_t)pp;
    size_t pages = size / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        map_page(va, (void *)pa, rwxug_flags);
        va += PAGE_SIZE;  //next
        pa += PAGE_SIZE;  //next
    }

    return (void *)vma;
}

/**
 * @brief Allocates and maps a range of physical pages to a virtual address range.
 *
 * @param vma         Starting virtual address to map.
 * @param size        Size of the range to allocate and map.
 * @param rwxug_flags Flags specifying access permissions
 * @return The starting virtual address vma.
 *
 * @sideeffects:
 *  Modifies the current page table.
 */
void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    size_t pages = size / PAGE_SIZE;

    uintptr_t va = vma;

    for (size_t i = 0; i < pages; i++) {
        void *pp = alloc_phys_page();  
        map_page(va, pp, rwxug_flags); // map to va
        va += PAGE_SIZE;               //next page
    }
    return (void *)vma;
}

/**
 * @brief Sets the permission flags of a mapped virtual address range.
 * This function walks through the pt entries of the specified va range 
 * and updates each leaf PTE's flags.
 *
 * @param vp           Starting virtual address of the range.
 * @param size         Size of the range.
 * @param rwxug_flags  New permission flags.
 *
 * @sideeffects
 * Modifies page table flags.
 */
void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    uintptr_t va = (uintptr_t)vp;
    size = ROUND_UP(size, PAGE_SIZE);
    size_t pages = size / PAGE_SIZE;
    struct pte *pt2 = active_space_ptab();

    for (size_t i = 0; i < pages; i++) {
        unsigned vpn2 = VPN2(va);
        unsigned vpn1 = VPN1(va);
        unsigned vpn0 = VPN0(va);

        if (!PTE_VALID(pt2[vpn2])) {
            panic("set_range_flags: pt2 invalid"); //invalid
        }

        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[vpn2].ppn << 12);
        if (!PTE_VALID(pt1[vpn1])) {
            panic("set_range_flags: pt1 invalid");
        }

        struct pte *pt0 = (struct pte *)((uintptr_t)pt1[vpn1].ppn << 12);
        if (!PTE_VALID(pt0[vpn0]) || !PTE_LEAF(pt0[vpn0])) {
            panic("set_range_flags: pt0 not a leaf");
        }

        // update flags
        pt0[vpn0].flags = rwxug_flags | PTE_V;  //pt0[vpn0] is struct pte
        va += PAGE_SIZE;
    }
}

/**
 * @brief Unmaps and frees a range of virtual addresses, traverses the 
 * page table entries of the specified virtual address range and unmaps each virtual page. 
 *
 * @param vp    Starting virtual address of the range to unmap.
 * @param size  Size of the range.
 *
 * @sideeffects: None.
 */
void unmap_and_free_range(void * vp, size_t size) {
    uintptr_t va = (uintptr_t)vp;
    size = ROUND_UP(size, PAGE_SIZE);
    size_t pages = size / PAGE_SIZE;

    struct pte *pt2 = active_space_ptab();

    for (size_t i = 0; i < pages; i++) {
        unsigned vpn2 = VPN2(va);
        unsigned vpn1 = VPN1(va);
        unsigned vpn0 = VPN0(va);

        if (!PTE_VALID(pt2[vpn2])) {
            panic("unmap_and_free_range: pt2 invalid");
        }

        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[vpn2].ppn << 12);
        if (!PTE_VALID(pt1[vpn1])) {
            panic("unmap_and_free_range: pt1 invalid");
        }

        struct pte *pt0 = (struct pte *)((uintptr_t)pt1[vpn1].ppn << 12);
        if (!PTE_VALID(pt0[vpn0]) || !PTE_LEAF(pt0[vpn0])) {
            panic("unmap_and_free_range: pt0 not a valid mapping");
        }

        // get pp address and free it
        void *pp = (void *)((uintptr_t)pt0[vpn0].ppn << 12);
        free_phys_page(pp);
        // clear mapping
        pt0[vpn0] = null_pte();
        va += PAGE_SIZE; //next
    }
    sfence_vma();
}

/**
 * @brief Allocates a single physical page.
 *
 * @return Pointer to the allocated physical page, or NULL if out of memory.
 *
 * @sideeffects: None
 */
void * alloc_phys_page(void) {
    return alloc_phys_pages(1);
}

/**
 * @brief Frees a single physical page.
 *
 * @param pp Pointer to the physical page to free.
 *
 * @sideeffects: Adds one page back to the free page list.
 */
void free_phys_page(void * pp) {
    free_phys_pages(pp, 1);
}






/**
 * @brief Allocates a block of physical pages using best-fit strategy.
 * Searches the global free page chunk list for the smallest chunk that fits pages cnt. 
 * If a perfect fit is found, it is removed directly.
 * Otherwise, the chunk is split, and the remaining part stays in the list.
 *
 * @param cnt Number of pages to allocate.
 * @return Pointer to the start of the allocated physical memory.
 *
 * @sideeffects: None
 */
void * alloc_phys_pages(unsigned int cnt) {
    if (cnt == 0) return NULL;
    struct page_chunk ** prev_ptr = &free_chunk_list;
    struct page_chunk * curr = free_chunk_list;
    struct page_chunk * best = NULL;
    struct page_chunk ** best_prev_ptr = NULL;

    // search
    while (curr != NULL) {
        if (curr->pagecnt >= cnt) {
            if (best == NULL || curr->pagecnt < best->pagecnt) {
                best = curr;
                best_prev_ptr = prev_ptr;
                if (curr->pagecnt == cnt) break;  // perfect fit
            }
        }
        prev_ptr = &curr->next; //prev_ptr: address of prev pointer!
        curr = curr->next;
    }

    if (best == NULL) {
        panic("alloc_phys_pages: no chunk big enough");
    }

    uintptr_t result = (uintptr_t)best;
    if (best->pagecnt == cnt) {
        *best_prev_ptr = best->next; //perfect fit
    } else { //split
        best->pagecnt -= cnt;
        struct page_chunk * remain = (struct page_chunk *)((uintptr_t)best + cnt * PAGE_SIZE);
        remain->pagecnt = best->pagecnt;
        remain->next = best->next;
        *best_prev_ptr = remain;
    }

    return (void *)result;
}



/**
 * @brief Frees a block of continuous physical pages.
 *
 * @param pp   Pointer to the first page in the block.
 * @param cnt  Number of pages to free.
 *
 * @sideeffects: Modifies the global free list structure.
 */
void free_phys_pages(void * pp, unsigned int cnt) {
    if (cnt == 0) return;

    // set as a new chunk
    struct page_chunk *new_chunk = (struct page_chunk *)pp;
    new_chunk->pagecnt = cnt;
    new_chunk->next = NULL;

    // insert to linked list
    struct page_chunk **prev = &free_chunk_list;
    struct page_chunk *curr = free_chunk_list;
    uintptr_t new_addr = (uintptr_t)new_chunk;

    while (curr != NULL && (uintptr_t)curr < new_addr) {
        prev = &curr->next;
        curr = curr->next;
    }
    // update list
    new_chunk->next = curr;
    *prev = new_chunk;
}



/**
 * @brief Returns the total number of free physical pages available.
 *
 *
 * @return Total number of free physical pages.
 *
 * @sideeffects: None
 */
unsigned long free_phys_page_count(void) {
    unsigned long total = 0;
    struct page_chunk *curr = free_chunk_list;

    while (curr) {
        total += curr->pagecnt;
        curr = curr->next;
    }
    return total;
}


/**
 * @brief Handles a user-mode page fault by lazily allocating and mapping a page.
 *
 * @param tfr Pointer to the current trap frame (not used in this handler).
 * @param vma The faulting virtual address that triggered the page fault.
 *
 * @return 1 if the page fault was handled successfully; 0 if failed.
 *
 * @sideeffect: None
 */
int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    if (vma < UMEM_START_VMA || vma >= UMEM_END_VMA) {
        return 0; // out of user space
    }

    void *pp = alloc_phys_page();
    if (pp == NULL) {
        return 0; // no enough space
    }
    map_page(vma, pp, PTE_R | PTE_W | PTE_U); //map 

    return 1; 
}


mtag_t active_space_mtag(void) {
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte * ptab, unsigned int asid) {
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte * mtag_to_ptab(mtag_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void * p) {
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags) {
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)
    };
}

static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag) {
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)
    };
}


static inline struct pte null_pte(void) {
    return (struct pte) { };
}



//added by cp3 extra
/**
 * @brief Validate whether a virtual memory range is accessible with specific permissions.
 * 
 * @param vp          Pointer to the start of the virtual memory.
 * @param len         Length of the memory region to check.
 * @param rwxu_flags  Required permission bits.
 * 
 * @return int        Returns 0 if the range is accessible; -1 if not.
 * @sideeffects:
 * None. 
 */
int validate_vptr(const void *vp, size_t len, int rwxu_flags) {
    uintptr_t start = (uintptr_t)vp;
    uintptr_t end = start + len;

    if (end < start || !wellformed(start) ) return -1; // Prevent overflow 
    if (!wellformed(end - 1)) return -1;

    if (start < UMEM_START_VMA || end > UMEM_END_VMA) return -1;

    struct pte *pt2 = active_space_ptab();
    uintptr_t va = ROUND_DOWN(start, PAGE_SIZE);
    for (; va < end; va += PAGE_SIZE)  {
        unsigned vpn2 = VPN2(va);
        unsigned vpn1 = VPN1(va);
        unsigned vpn0 = VPN0(va);

        if (!PTE_VALID(pt2[vpn2])) return -1;
        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[vpn2].ppn << 12);

        if (!PTE_VALID(pt1[vpn1])) return -1;
        struct pte *pt0 = (struct pte *)((uintptr_t)pt1[vpn1].ppn << 12);

        struct pte pte = pt0[vpn0];
        if (!PTE_VALID(pte) || !PTE_LEAF(pte)) return -1;

        // check flag
        if ((pte.flags & rwxu_flags) != rwxu_flags) return -1;
    }

    return 0;
}


/**
 * @brief Validate whether a null-terminated virtual string is accessible with specific permissions.
 * 
 * @param vs        Pointer to the beginning of the virtual string.
 * @param ug_flags  Required access flag.
 * 
 * @return int      Returns 0 if the entire string is valid and null-terminated; -1 if not.
 * @sideeffects:
 * None.
 */
int validate_vstr(const char *vs, int ug_flags) {
    uintptr_t va = (uintptr_t)vs;

    if (!wellformed(va)) return -1;
    if (va < UMEM_START_VMA || va >= UMEM_END_VMA) return -1; //###

    struct pte *pt2 = active_space_ptab();

    while (1) {
        unsigned vpn2 = VPN2(va);
        unsigned vpn1 = VPN1(va);
        unsigned vpn0 = VPN0(va);

        if (!PTE_VALID(pt2[vpn2])) return -1;
        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[vpn2].ppn << 12);

        if (!PTE_VALID(pt1[vpn1])) return -1;
        struct pte *pt0 = (struct pte *)((uintptr_t)pt1[vpn1].ppn << 12);

        struct pte pte = pt0[vpn0];
        if (!PTE_VALID(pte) || !PTE_LEAF(pte)) return -1;
        if ((pte.flags & ug_flags) != ug_flags) return -1;

        uintptr_t page_end = ROUND_UP(va + 1, PAGE_SIZE); //offset
        while (va < page_end) {
            if (va >= UMEM_END_VMA) return -1;  // check edge
            const char *p = (const char *)va;
            if (*p == '\0') return 0;  
            va++;
            if (!wellformed(va)) return -1;
        }
    }
}



//helper func
int copyin(void *dst, const void *src, size_t size) {
    memcpy(dst, src, size);
    return 0;
}

//helper func
int copyout(void *dst, const void *src, size_t size) {
    memcpy(dst, src, size);
    return 0;
}
