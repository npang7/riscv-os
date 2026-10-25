// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

#define RTC_blcsz 8     // block size of the RTC data

// INTERNAL TYPE DEFINITIONS
// 

// define the RTC registers structure
struct rtc_regs {
    // TODO
    volatile uint32_t time_low;     // low 32-bit timestamp
    volatile uint32_t time_high;    // high 32-bit timestamp
};

// define the RTC device structure
struct rtc_device {
    // TODO
    struct rtc_regs * regs; // pointer to the registers of the RTC device
    struct io io;           // the I/O interface of the RTC device
    uint16_t instno;        // device instance number returned after registering the device
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct io ** ioptr, void * aux);
static void rtc_close(struct io * io);
static int rtc_cntl(struct io * io, int cmd, void * arg);
static long rtc_read(struct io * io, void * buf, long bufsz);

static uint64_t read_real_time(struct rtc_regs * regs);

// EXPORTED FUNCTION DEFINITIONS
// 

/* void rtc_attach(void * mmio_base)
 * Inputs: void * mmio_base - base address of the memory-mapped RTC registers
 * Outputs: None
 * Description: This function registers the device with the system, set up the I/O interface, and its memory-mapped registers.
 * Side Effects: New memory is allocated for the new RTC device.
 */
void rtc_attach(void * mmio_base) {
    // TODO
    // check whether the input is valid
    if (mmio_base == NULL) {
        return;
    }

    // allocate memory for the new RTC device
    struct rtc_device * rtc = (struct rtc_device *) kmalloc(sizeof(struct rtc_device));
    // if the allocation fails, return
    if (rtc == NULL) {
        return;
    }
    // initialize newly allocated memory to 0
    memset(rtc, 0, sizeof(struct rtc_device));

    // assign the MMIO base address to the register pointer in the device
    rtc->regs = (struct rtc_regs *) mmio_base;

    // allocate memory for I/O interface structure of the new RTC device
    struct iointf * rtc_intf = (struct iointf *) kmalloc(sizeof(struct iointf));
    // if the allocation fails, free previously allocated memory and return
    if (rtc_intf == NULL) {
        kfree(rtc);
        return;
    }
    // set up the I/O interface of the RTC device
    rtc_intf->close = rtc_close;
    rtc_intf->cntl = rtc_cntl;
    rtc_intf->read = rtc_read;
    rtc->io.intf = rtc_intf;

    // initialize the reference count
    rtc->io.refcnt = 0;

    // register the RTC device and set the instance number
    rtc->instno = register_device("rtc", rtc_open, rtc);
}

/* int rtc_open(struct io ** ioptr, void * aux)
 * Inputs: struct io ** ioptr - pointer to store the reference to the RTC IO structure for an RTC instance
 *         void * aux - pointer to the RTC device structure
 * Outputs: return 0 if succeed, otherwise return an error code.
 * Description: This function associates an IO reference with the RTC device, allowing it to be used by other system components.
 *              And it ensures the device is properly referenced before returning control.
 * Side Effects: None
 */
int rtc_open(struct io ** ioptr, void * aux) {
    // TODO
    // check whether the inputs are valid
    if (ioptr == NULL || aux == NULL) {
        return -EINVAL;
    }

    struct rtc_device * rtc = (struct rtc_device *) aux;
    // associate an IO reference with the RTC device
    *ioptr = &rtc->io;
    // increase the reference count
    rtc->io.refcnt += 1;

    return 0;
}

/* void rtc_close(struct io * io)
 * Inputs: struct io * io - pointer to the I/O interface of the RTC device
 * Outputs: None
 * Description: This function closes the RTC.
 * Side Effects: Memory is freed when the device is completely closed.
 */
void rtc_close(struct io * io) {
    // TODO
    // check whether the input is valid
    if (io == NULL) {
        return;
    }

    // get the pointer to the related RTC device
    struct rtc_device * const rtc = (void *) io - offsetof(struct rtc_device, io);

    // if the reference count becomes 0, free the memory
    if (io->refcnt == 0) {
        if (io->intf != NULL) {
            kfree((void *) io->intf);
            kfree(rtc);
        }
    }
}

/* int rtc_cntl(struct io * io, int cmd, void * arg)
 * Inputs: struct io * io - pointer to the I/O interface of the RTC device
 *         int cmd - command identifier for the requested operation
 *         void * arg - argument for the command (optional)
 * Outputs: return the block size value if cmd matches the desired command, otherwise return an error code.
 * Description: This function handles control and miscellaneous IO operations. In this implementation, this
 *              function only supports querying the block size of the RTC data.
 * Side Effects: None
 */
int rtc_cntl(struct io * io, int cmd, void * arg) {
    // TODO
    // check whether the input is valid
    if (io == NULL) {
        return -EINVAL;
    }

    // return the block size value if cmd matches the desired command
    if (cmd == IOCTL_GETBLKSZ) {
        return RTC_blcsz;
    }

    // if not match, return an error code 
    return -ENOTSUP;
}

/* long rtc_read(struct io * io, void * buf, long bufsz)
 * Inputs: struct io * io - pointer to the I/O interface of the RTC device
 *         void * buf - pointer to the buffer where the timestamp will be stored
 *         long bufsz - size of the buffer in bytes
 * Outputs: return the size of data written; return an error code if fail.
 * Description: This function gets the current real-time clock value and copies it to the given buffer.
 * Side Effects: None
 */
long rtc_read(struct io * io, void * buf, long bufsz) {
    // TODO
    // check whether the inputs are valid
    if (io == NULL || buf == NULL) {
        return -EINVAL;
    }

    // get the pointer to the related RTC device
    struct rtc_device * const rtc = (void *) io - offsetof(struct rtc_device, io);

    // make sure that rtc and rtc->regs are valid
    if (rtc == NULL || rtc->regs == NULL) {
        return -EINVAL;
    }

    // ensure that the buffer can hold the 64-bit timestamp
    if (bufsz < sizeof(uint64_t)) {
        return -ENOTSUP;
    }

    // read the 64-bit real-time clock value
    uint64_t timestamp = read_real_time(rtc->regs);
    // write the timestamp to the buffer
    memcpy(buf, &timestamp, sizeof(uint64_t));

    // return the size of data written
    return sizeof(uint64_t);
}

/* uint64_t read_real_time(struct rtc_regs * regs)
 * Inputs: struct rtc_regs * regs - pointer to the memory-mapped RTC registers
 * Outputs: return the full 64-bit timestamp; return an error code if fail.
 * Description: This function returns the full 64-bit timestamp, which is a helper function for rtc_read.
 * Side Effects: None
 */
uint64_t read_real_time(struct rtc_regs * regs) {
    // TODO
    // check whether the input is valid
    if (regs == NULL) {
        return -EINVAL;
    }

    uint32_t timelow, timehigh;
    // first read the low 32-bit
    timelow = regs->time_low;
    // then read the high 32-bit
    timehigh = regs->time_high;
    // combine these to form the full 64-bit timestamp
    uint64_t timestamp = ((uint64_t)timehigh << 32) | timelow;  // move timehigh to the high 32 bits

    return timestamp;
}
