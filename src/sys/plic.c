// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

#define UINT32_bits_num 32          // each uint32_t contains 32 bits
#define ALLONE_32bits 0xFFFFFFFF    // let all the 32 bits to be 1
#define ALLZERO_32bits 0x00000000   // let all the 32 bits to be 0

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 

struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT];
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32];
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32];
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;
				uint32_t claim;
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);
static int plic_source_pending(uint_fast32_t srcno);
static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);
static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);
static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 1; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno < PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	// FIXME: Hardwired S-mode hart 0
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	// FIXME: Hardwired S-mode hart 0
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

/* void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
 * Inputs: uint_fast32_t srcno - interrupt source number
 *         uint_fast32_t level - priority level of the interrupt source
 * Outputs: None
 * Description: This function sets the priority level for a specific interrupt source.
 * Side Effects: None
 */
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here
    // set the priority level for the specific interrupt source
    PLIC.priority[srcno] = level;
}

/* int plic_source_pending(uint_fast32_t srcno)
 * Inputs: uint_fast32_t srcno - interrupt source number
 * Outputs: return 1 if the specific interrupt source is pending, 0 otherwise.
 * Description: This function checks if an interrupt source is pending by inspecting the pending array.
 * Side Effects: None
 */
static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here
    // check whether the interrupt source number is valid
    if (srcno <= 0 || srcno >= PLIC_SRC_CNT) {
        return 0;
    }

    // find where the pending bit is (in which byte and which bit)
    uint32_t byte = srcno/UINT32_bits_num;
    uint32_t bit = srcno%UINT32_bits_num;

	// check if the specific bit is 1
	if (PLIC.pending[byte] & (1U<<bit)) {
        // the source is pending
		return 1;
	}
    else {
		return 0;
	}
}

/* void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
 * Inputs: uint_fast32_t ctxno - context number
 *         uint_fast32_t srcno - interrupt source number
 * Outputs: None
 * Description: This function enables a specific interrupt source for a given context.
 * Side Effects: None
 */
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
    // check whether the interrupt source number is valid
    if (srcno <= 0 || srcno >= PLIC_SRC_CNT) {
        return;
    }

    // find where the enable bit is (in which byte and which bit)
	uint32_t byte = srcno/UINT32_bits_num;
    uint32_t bit = srcno%UINT32_bits_num;

    // set the specific enable bit to be 1
    PLIC.enable[ctxno][byte] |= (1U<<bit);
}

/* void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
 * Inputs: uint_fast32_t ctxno - context number
 *         uint_fast32_t srcid - interrupt source number
 * Outputs: None
 * Description: This function disables a specific interrupt source for a given context.
 * Side Effects: None
 */
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	// FIXME your code goes here
    // find where the enable bit is (in which byte and which bit)
	uint32_t byte = srcid/UINT32_bits_num;
    uint32_t bit = srcid%UINT32_bits_num;

    // set the specific enable bit to be 0
    PLIC.enable[ctxno][byte] &= ~(1U<<bit);
}

/* void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
 * Inputs: uint_fast32_t ctxno - context number
 *         uint_fast32_t level - priority level of the interrupt source
 * Outputs: None
 * Description: This function sets the interrupt priority threshold for a specific context.
 * Side Effects: None
 */
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	// FIXME your code goes here
	// check whether the context number is valid
    if (ctxno < 0 || ctxno >= PLIC_CTX_CNT) {
        return;
    }

    // set the interrupt priority threshold for the specific context
    PLIC.ctx[ctxno].threshold = level;
}

/* uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
 * Inputs: uint_fast32_t ctxno - context number
 * Outputs: return the interrupt ID of the pending interrupt source with highest priority,
 *          and return 0 if there are no interrupts pending.
 * Description: This function claims an interrupt for a given context.
 * Side Effects: None
 */
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here
    // check whether the context number is valid
    if (ctxno < 0 || ctxno >= PLIC_CTX_CNT) {
        return 0;
    }

    // return the interrupt ID in the claim register
    return PLIC.ctx[ctxno].claim;
}

/* void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
 * Inputs: uint_fast32_t ctxno - context number
 *         uint_fast32_t srcno - interrupt source number
 * Outputs: None
 * Description: This function completes the handling of an interrupt for a given context
 *              by writing the interrupt source number back to the claim register.
 * Side Effects: None
 */
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
    // check whether the interrupt source number is valid
    if (srcno <= 0 || srcno >= PLIC_SRC_CNT) {
        return;
    }

    // check whether the context number is valid
    if (ctxno < 0 || ctxno >= PLIC_CTX_CNT) {
        return;
    }

    // write the interrupt ID into the claim register
    PLIC.ctx[ctxno].claim = srcno;
}

/* void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
 * Inputs: uint_fast32_t ctxno - context number
 * Outputs: None
 * Description: This function enables all interrupt sources for a given context.
 * Side Effects: None
 */
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	// check whether the context number is valid
    if (ctxno < 0 || ctxno >= PLIC_CTX_CNT) {
        return;
    }

    uint32_t i = 0;
    // iterate through all items in the enable array entry for a specific context, setting each bit of each item to 1
    for (; i < 32; i++) {   // in the plic_regs structure, each entry has 32 items
        PLIC.enable[ctxno][i] = ALLONE_32bits;
    }
}

/* void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
 * Inputs: uint_fast32_t ctxno - context number
 * Outputs: None
 * Description: This function disables all interrupt sources for a given context.
 * Side Effects: None
 */
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
    uint32_t i = 0;
    // iterate through all items in the enable array entry for a specific context, setting each bit of each item to 0
    for (; i < 32; i++) {   // in the plic_regs structure, each entry has 32 items
        PLIC.enable[ctxno][i] = ALLZERO_32bits;
    }
}
