// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"
#include "thread.h" // add for using condition variables

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.

        struct virtq_desc desc[1];
    } vq;

    // bufcnt is the number of bytes left in buffer. The usable bytes are
    // between buf+0 and buf+bufcnt. (We read from the end of the buffer.)

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];

    // add a condition variable representing the VirtIO Entropy device has finished processing the request
    struct condition viorng_finish;
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

/* void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
 * Inputs: volatile struct virtio_mmio_regs * regs - pointer to the memory-mapped I/O registers of the VirtIO Entropy device
 *         int irqno - interrupt number of the VirtIO Entropy device
 * Outputs: None
 * Description: This function initializes the VirtIO Entropy device with the necessary IO operation functions and sets the
 *              required feature bits. It also fills out the descriptors in the virtqueue struct. It attaches the virtqueues
 *              and register the device.
 * Side Effects: New memory is allocated for the new VirtIO Entropy device.
 */
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    //           FIXME add additional declarations here if needed
    // check whether the input is valid
    if (regs == NULL) {
        return;
    }


    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    //           FIXME Finish viorng initialization here! 
    // allocate memory for the new VirtIO Entropy device
    struct viorng_device * viorng = kmalloc(sizeof(struct viorng_device));
    // if the allocation fails, return
    if (viorng == NULL) {
        return;
    }
    // initialize newly allocated memory to 0
    memset(viorng, 0, sizeof(struct viorng_device));

    // allocate memory for I/O interface structure of the new VirtIO Entropy device
    struct iointf * viorng_intf = (struct iointf *) kmalloc(sizeof(struct iointf));
    // if the allocation fails, free previously allocated memory and return
    if (viorng_intf == NULL) {
        kfree(viorng);
        return;
    }

    // set up the I/O interface of the new VirtIO Entropy device
    viorng_intf->close = viorng_close;
    viorng_intf->read = viorng_read;
    viorng->io.intf = viorng_intf;

    // initialize all other necessary fields in the new VirtIO Entropy device
    viorng->regs = regs;            // initialize the MMIO registers
    viorng->irqno = irqno;          // initialize the interrupt number
    // register the new VirtIO Entropy device and get its instance number
    viorng->instno = register_device(VIORNG_NAME, viorng_open, viorng);
    viorng->io.refcnt = 0;          // initialize the reference count
    viorng->vq.last_used_idx = 0;   // initialize the latest index of the used virtqueue
    viorng->bufcnt = 0;             // initialize the number of bytes left in buffer

    // attach the virtq_avail and virtq_used structs
    // set qid to be 0
    // VIRTQ_AVAIL_SIZE and VIRTQ_USED_SIZE are both 1 in the viorng_device definition
    virtio_attach_virtq(viorng->regs, 0, 1, (uint64_t)viorng->vq.desc, (uint64_t)&viorng->vq.used, (uint64_t)&viorng->vq.avail);

    // register the interrupt service routine
    //intr_install_isr(irqno, viorng_isr, viorng);

    // fill out the descriptors in the virtqueue struct
    viorng->vq.desc[0].addr = (uint64_t)viorng->buf;
    viorng->vq.desc[0].len = VIORNG_BUFSZ;
    viorng->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;      // this buffer is device write-only
    viorng->vq.desc[0].flags &= ~VIRTQ_DESC_F_NEXT;     // there doesn't exist a next descriptor
    viorng->vq.desc[0].next = 0;

    // initialize the newly added condition
    condition_init(&viorng->viorng_finish, "viorng_finish");


    // fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;    
    //           fence o,oi
    __sync_synchronize();
}

/* int viorng_open(struct io ** ioptr, void * aux)
 * Inputs: struct io ** ioptr - pointer to return the I/O interface of the VirtIO Entropy device
 *         void * aux - auxiliary pointer to pass the device in
 * Outputs: return 0 if succeeds
 * Description: This function makes the virtq_avail and virtq_used queues available for use. It enables the interrupt
 *              source for the device, with the correct ISR, and return the IO operations via ioptr.
 * Side Effects: None
 */
int viorng_open(struct io ** ioptr, void * aux) {
    //           FIXME your code here
    // check whether the inputs are valid
    if (ioptr == NULL || aux == NULL) {
        return -EINVAL;
    }

    // aux is the pointer to the VirtIO Entropy device
    struct viorng_device * viorng = aux;

    // make the virtq_avail and virtq_used queues available for use
    // the qid we previously set is 0
    virtio_enable_virtq(viorng->regs, 0);

    // enable the interrupt source for the device
    enable_intr_source(viorng->irqno, VIORNG_IRQ_PRIO, viorng_isr, viorng);

    // return the IO operations to ioptr
    *ioptr = &viorng->io;

    // increment the reference count
    viorng->io.refcnt++;

    // return 0 on success
    return 0;
}

/* void viorng_close(struct io * io)
 * Inputs: struct io * io - pointer to the I/O interface of the VirtIO Entropy device
 * Outputs: None
 * Description: This function resets the virtq_avail and virtq_used queues and prevents further interrupts.
 * Side Effects: None
 */
void viorng_close(struct io * io) {
    //           FIXME your code here
    // check whether the input is valid
    if (io == NULL) {
        return;
    }

    // get the VirtIO Entropy device instance
    struct viorng_device * const viorng = (void*)io - offsetof(struct viorng_device, io);

    // reset the virtq_avail and virtq_used queues
    // the qid we previously set is 0
    virtio_reset_virtq(viorng->regs, 0);

    // prevent further interrupts
    disable_intr_source(viorng->irqno);
}

/* long viorng_read(struct io * io, void * buf, long bufsz)
 * Inputs: struct io * io - pointer to the I/O interface of the VirtIO Entropy device
 *         void * buf - buffer to store data read from the VirtIO Entropy device
 *         long bufsz - the number of bytes we need to read
 * Outputs: return the number of bytes of randomness successfully obtained
 * Description: This function reads up to bufsz bytes from the VirtIO Entropy device and writes them to buf.
 * Side Effects: The memory of the device buffer is changed.
 */
long viorng_read(struct io * io, void * buf, long bufsz) {
    //           FIXME your code here
    // check whether the inputs are valid
    if (io == NULL || buf == NULL) {
        return -EINVAL;
    }

    // get the device instance contains the io pointer
    struct viorng_device * const viorng = (void*)io - offsetof(struct viorng_device, io);

    // if we need to read 0 bytes, just return
    if (bufsz == 0) {
        return 0;
    }

    // if the bufsz is longer than the device buffer length, simply set it equal the device buffer length
    if (VIORNG_BUFSZ < bufsz) {
        bufsz = VIORNG_BUFSZ;
    }

    // if there is not enough data available in the device, request new data from the device
    if (viorng->bufcnt - bufsz < 0) {
        // update virtq_avail
        // VIRTQ_SIZE is 1
        viorng->vq.avail.ring[viorng->vq.avail.idx % 1] = 0;  // qid is 0
        viorng->vq.avail.idx++;

        // notify the device that there is a new request
        // the qid is always 0
        virtio_notify_avail(viorng->regs, 0);

        // avoid race conditions
        int pie = disable_interrupts();
        // wait on the newly added condition, which means the device has filled with new data
        while (viorng->vq.last_used_idx == viorng->vq.used.idx) {
            condition_wait(&viorng->viorng_finish);
            // memory barrier function
            __sync_synchronize();
        }
        restore_interrupts(pie);

        // update the the latest index of the used virtqueue
        viorng->vq.last_used_idx = viorng->vq.used.idx;
        // memory barrier function
        __sync_synchronize();
        
        // the number of bytes left in buffer equals VIORNG_BUFSZ now, since the device buffer is filled with new data
        viorng->bufcnt = VIORNG_BUFSZ;
    }

    uint8_t * buffer_ptr = (uint8_t *) buf;  // pointer to track buffer location

    // update the number of bytes left in buffer
    viorng->bufcnt -= bufsz;

    // copy data to the given buffer
    memcpy(buffer_ptr, &viorng->buf[viorng->bufcnt], bufsz);

    // return the number of bytes successfully read
    return bufsz;
}

/* void viorng_isr(int irqno, void * aux)
 * Inputs: int irqno - interrupt number of the VirtIO Entropy device
 *         void * aux - auxiliary pointer to pass the device in
 * Outputs: None
 * Description: This function sets the appropriate device registers and wakes the thread up after waiting for the
 *              device to finish servicing a request.
 * Side Effects: None
 */
void viorng_isr(int irqno, void * aux) {
    //           FIXME your code here
    // check whether the input is valid
    if (aux == NULL) {
        return;
    }

    // aux is the pointer to the VirtIO Entropy device
    struct viorng_device * const viorng = aux;

    // get the current interrupt status
    uint32_t status = viorng->regs->interrupt_status;

    // memory barrier function
    __sync_synchronize();

    // if the interrupt status is not set, return directly
    if (status == 0) {
        return;
    }

    // notify the device that events causing the interrupt have been handled
    viorng->regs->interrupt_ack = status;

    // broadcast the related condition
    condition_broadcast(&viorng->viorng_finish);
}
