// vioblk.c - VirtIO serial port (console)
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "error.h"
#include "string.h"
#include "assert.h"
#include "ioimpl.h"
#include "io.h"
#include "conf.h"

#include <limits.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// set the virtqueue size in the vioblk device to be 4
#define VIOBLK_VIRTQ_SIZE 4

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_BLK_F_TOPOLOGY       10
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_BLK_F_DISCARD        13
#define VIRTIO_BLK_F_WRITE_ZEROES   14

// VirtIO block device request types
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_T_GET_LIFETIME 10
#define VIRTIO_BLK_T_DISCARD 11
#define VIRTIO_BLK_T_WRITE_ZEROES 13
#define VIRTIO_BLK_T_SECURE_ERASE 14

// status bytes in the request
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2


// we piece together a complete request with 3 descriptors:
// the first descriptor stores type, reserved and sector;
// the second descriptor stores the data;
// the last descriptor stores the status byte.
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// the VirtIO block device structure
struct vioblk_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;

    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(VIOBLK_VIRTQ_SIZE)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(VIOBLK_VIRTQ_SIZE)];
        };

        // we piece together a complete request with 3 descriptors:
        // the first descriptor stores the request below;
        // the second descriptor stores the data buffer;
        // the last descriptor stores the status byte.
        struct virtq_desc desc[3];
    } vq;

    struct virtio_blk_req request;  // the top three elements in the vioblk request
    uint8_t status; // the status byte in the vioblk request

    uint32_t block_size;    // size of each block in the vioblk device

    char * vioblk_buf;  // the block buffer storing data in the vioblk device

    // the condition variable representing the vioblk device has finished processing the request
    struct condition vioblk_finish;

    struct lock vioblk_lock;    // lock for vioblk device
};


// INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io ** ioptr, void * aux);
static void vioblk_close(struct io * io);

static long vioblk_readat (
    struct io * io,
    unsigned long long pos,
    void * buf,
    long bufsz);

static long vioblk_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf,
    long len);

static int vioblk_cntl (
    struct io * io, int cmd, void * arg);

static void vioblk_isr(int srcno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.

/* void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno)
 * Inputs: volatile struct virtio_mmio_regs * regs - pointer to the memory-mapped I/O registers of the vioblk device
 *         int irqno - interrupt number of the vioblk device
 * Outputs: None
 * Description: This function initializes the virtio block device with the necessary IO operation functions and sets
 *              the required feature bits. It also fills out the descriptors in the virtq struct. It attaches the
 *              virtq_avail and virtq_used structs using the virtio_attach_virtq function. Finally, the ISR and device
 *              are registered.
 * Side Effects: New memory is allocated for the new vioblk device.
 */
void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    // notify the device that the driver is ready to start feature negotiation
    regs->status |= VIRTIO_STAT_DRIVER;
    // memory barrier function
    __sync_synchronize();


    // declare the required variables
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    uint32_t blksz;

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // function negotiation has been successful
    regs->status |= VIRTIO_STAT_FEATURES_OK;
    // memory barrier function
    __sync_synchronize();

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert (((blksz - 1) & blksz) == 0);

    // FIX ME
    // allocate memory for the new vioblk device
    struct vioblk_device * vioblk = kmalloc(sizeof(struct vioblk_device));
    // if the allocation fails, return
    if (vioblk == NULL) {
        return;
    }
    // initialize newly allocated memory to 0
    memset(vioblk, 0, sizeof(struct vioblk_device));

    // allocate memory for I/O interface structure of the new vioblk device
    struct iointf * vioblk_intf = (struct iointf *) kmalloc(sizeof(struct iointf));
    // if the allocation fails, free previously allocated memory and return
    if (vioblk_intf == NULL) {
        kfree(vioblk);
        return;
    }
    // initialize newly allocated memory to 0
    memset(vioblk_intf, 0, sizeof(struct iointf));

    // allocate memory for the vioblk buffer
    vioblk->vioblk_buf = (char *) kmalloc(blksz);
    // if the allocation fails, free previously allocated memory and return
    if (vioblk->vioblk_buf == NULL) {
        kfree(vioblk_intf);
        kfree(vioblk);
        return;
    }
    // initialize newly allocated memory to 0
    memset(vioblk->vioblk_buf, 0, blksz);

    // set up the I/O interface of the new vioblk device
    vioblk_intf->close =  vioblk_close;
    vioblk_intf->readat = vioblk_readat;
    vioblk_intf->writeat = vioblk_writeat;
    vioblk_intf->cntl = vioblk_cntl;
    vioblk->io.intf = vioblk_intf;

    // initialize all other necessary fields in the new vioblk device
    vioblk->regs = regs;            // initialize the MMIO registers
    vioblk->irqno = irqno;          // initialize the interrupt number
    // register the new vioblk device and get its instance number
    vioblk->instno = register_device(VIOBLK_NAME, vioblk_open, vioblk);
    vioblk->io.refcnt = 0;          // initialize the reference count
    vioblk->vq.last_used_idx = 0;   // initialize the latest index of the used virtqueue
    vioblk->block_size = blksz;     // initialize the block size

    // attach the virtq_avail and virtq_used structures
    // set qid to be 0
    virtio_attach_virtq(vioblk->regs, 0, VIOBLK_VIRTQ_SIZE, (uint64_t)vioblk->vq.desc, (uint64_t)&vioblk->vq.used, (uint64_t)&vioblk->vq.avail);

    // register the ISR
    enable_intr_source(vioblk->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vioblk);

    // initialize the vioblk_finish condition
    condition_init(&vioblk->vioblk_finish, "vioblk_finish");


    // fill out the three descriptors in the virtqueue structure

    // the first descriptor stores type, reserved and sector
    vioblk->vq.desc[0].addr  = (uint64_t)&vioblk->request;
    vioblk->vq.desc[0].len   = sizeof(vioblk->request);
    vioblk->vq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    vioblk->vq.desc[0].next = 1;    // the index of the next descriptor is 1
    // the second descriptor stores the data buffer
    vioblk->vq.desc[1].addr  = (uint64_t)vioblk->vioblk_buf;
    vioblk->vq.desc[1].len   = vioblk->block_size;
    vioblk->vq.desc[1].flags = VIRTQ_DESC_F_NEXT;
    vioblk->vq.desc[1].next = 2;    // the index of the next descriptor is 2
    // the last descriptor stores the status byte
    vioblk->vq.desc[2].addr  = (uint64_t)&vioblk->status;
    vioblk->vq.desc[2].len   = sizeof(vioblk->status);
    vioblk->vq.desc[2].flags = VIRTQ_DESC_F_WRITE;  // this is the end and the status byte is written by the device


    // the driver is ready to start using the device
    regs->status |= VIRTIO_STAT_DRIVER_OK;    
    // memory barrier function
    __sync_synchronize();
}

/* int vioblk_open(struct io ** ioptr, void * aux)
 * Inputs: struct io ** ioptr - pointer to return the I/O interface of the vioblk device
 *         void * aux - auxiliary pointer to pass the device in
 * Outputs: return 0 if succeeds; else, return an error code
 * Description: This function performs necessary initialization for the VirtIO block device. It configures
 *              the relevant virtq (avail and used) queues so they are ready for use. It also enables the
 *              interupt line for the device and sets necessary flags. It returns the IO operations to ioptr.
 * Side Effects: None
 */
int vioblk_open(struct io ** ioptr, void * aux) {
    // check whether the inputs are valid
    if (ioptr == NULL || aux == NULL) {
        return -EINVAL;
    }

    // aux is the pointer to the vioblk device
    struct vioblk_device * vioblk = aux;

    // make the virtq_avail and virtq_used queues available for use
    // the qid we previously set is 0
    virtio_enable_virtq(vioblk->regs, 0);

    // return the IO operations to ioptr
    *ioptr = create_seekable_io(&vioblk->io);

    // increment the reference count
    vioblk->io.refcnt++;

    // initialize the vioblk lock
    lock_init(&vioblk->vioblk_lock);

    // return 0 on success
    return 0;
}

/* void vioblk_close(struct io * io)
 * Inputs: struct io * io - pointer to the I/O interface of the vioblk device
 * Outputs: None
 * Description: This function resets the virtqueues and disable future device interrupts.
 * Side Effects: None
 */
void vioblk_close(struct io * io) {
    // check whether the input is valid
    if (io == NULL) {
        return;
    }

    // get the vioblk device instance
    struct vioblk_device * vioblk = (void*)io - offsetof(struct vioblk_device, io);

    // reset the virtq_avail and virtq_used queues
    // the qid we previously set is 0
    virtio_reset_virtq(vioblk->regs, 0);

    // prevent further interrupts
    disable_intr_source(vioblk->irqno);
}

/* long vioblk_readat (struct io * io, unsigned long long pos, void * buf, long bufsz)
 * Inputs: struct io * io - pointer to the I/O interface of the vioblk device
 *         unsigned long long pos - the starting position for the read within the vioblk device
 *         void * buf - buffer to store data read from the vioblk device
 *         long bufsz - the number of bytes we need to read
 * Outputs: return the number of bytes read from the vioblk device
 * Description: This function reads bufsz number of bytes (must be aligned to VirtIO block size) from the disk and writes them to buf.
 * Side Effects: The memory of the device buffer is changed.
 */
long vioblk_readat (struct io * io, unsigned long long pos, void * buf, long bufsz) {
    // check whether the inputs are valid
    if ((io == NULL) || (buf == NULL)) {
        return -EINVAL;
    }

    // get the specified vioblk device based on the io pointer
    struct vioblk_device * vioblk = (void*)io - offsetof(struct vioblk_device, io);

    lock_acquire(&vioblk->vioblk_lock);

    // if bufsz is not aligned with the block size or 512, return an error code
    if (bufsz % vioblk->block_size != 0 || bufsz % 512 != 0) {
        lock_release(&vioblk->vioblk_lock);
        return -EINVAL;
    }

    // if we need to read 0 bytes, just return
    if (bufsz == 0) {
        lock_release(&vioblk->vioblk_lock);
        return 0;
    }


    // in vioblk_readat, the data buffer is device write-only
    vioblk->vq.desc[1].flags |= VIRTQ_DESC_F_WRITE;

    // set the specified request to read one block
    vioblk->request.type = VIRTIO_BLK_T_IN;  // this is a read request
    vioblk->request.reserved = 0;   // no use, just set 0
    vioblk->request.sector = pos / vioblk->block_size;  // find the specified sector to read
    
    unsigned long bytes_read = 0;   // bytes we have already read
    uint8_t * buffer_ptr = (uint8_t *)buf;  // pointer to track buffer location
    uint64_t start_pos = pos % vioblk->block_size;  // starting reading position in the block

    // loop until we have read bufsz number of bytes
    while (bufsz > bytes_read) {
        vioblk->status = 3;    // indicate that the request has not been completed
        
        // update virtq_avail
        vioblk->vq.avail.ring[vioblk->vq.avail.idx % VIOBLK_VIRTQ_SIZE] = 0;  // qid is 0
        vioblk->vq.avail.idx++;

        // notify the device that there is a new request
        // the qid is always 0
        virtio_notify_avail(vioblk->regs, 0);
        
        // avoid race conditions
        int pie = disable_interrupts();
        // wait on the vioblk_finish condition, which means the device has filled with new data
        while (vioblk->vq.last_used_idx == vioblk->vq.used.idx) {
            condition_wait(&vioblk->vioblk_finish);
            // memory barrier function
            __sync_synchronize();
        }
        restore_interrupts(pie);

        // update the the latest index of the used virtqueue
        vioblk->vq.last_used_idx = vioblk->vq.used.idx;
        // memory barrier function
        __sync_synchronize();

        // check device return status
        if (vioblk->status != VIRTIO_BLK_S_OK) {
            lock_release(&vioblk->vioblk_lock);
            return -EIO;
        }

        // copy data from the device buffer to the destination buffer
        unsigned long copy_size = vioblk->block_size - start_pos;
        // if the rest bytes to read is not enough
        if (copy_size > bufsz - bytes_read) {
            copy_size = bufsz - bytes_read;
        }
        // copy
        memcpy(buffer_ptr, &vioblk->vioblk_buf[start_pos], copy_size);

        // update to read the next block
        buffer_ptr += copy_size;
        bytes_read += copy_size;
        // read the next sector
        vioblk->request.sector++;
        // the rest of the block should be read from the beginning
        start_pos = 0;
    }

    lock_release(&vioblk->vioblk_lock);

    // return the number of bytes successfully read
    return bytes_read;
}

/* long vioblk_writeat (struct io * io, unsigned long long pos, const void * buf, long len)
 * Inputs: struct io * io - pointer to the I/O interface of the vioblk device
 *         unsigned long long pos - the starting position for the write within the vioblk device
 *         void * buf - buffer to store data to write to the vioblk device
 *         long len - the number of bytes we need to write
 * Outputs: return the number of bytes written to the vioblk device
 * Description: This function writes len bytes (must be aligned to VirtIO block size) from buf to the disk.
 * Side Effects: The memory of the device buffer is changed.
 */
long vioblk_writeat (struct io * io, unsigned long long pos, const void * buf, long len) {
    // check whether the inputs are valid
    if ((io == NULL) || (buf == NULL)) {
        return -EINVAL;
    }

    // get the specified vioblk device based on the io pointer
    struct vioblk_device * vioblk = (void*)io - offsetof(struct vioblk_device, io);

    lock_acquire(&vioblk->vioblk_lock);

    // if len is not aligned with the block size or 512, return an error code
    if (len % vioblk->block_size != 0 || len % 512 != 0) {
        lock_release(&vioblk->vioblk_lock);
        return -EINVAL;
    }

    // if we need to write 0 bytes, just return
    if (len == 0) {
        lock_release(&vioblk->vioblk_lock);
        return 0;
    }


    // in vioblk_writeat, the data buffer is not device write-only
    vioblk->vq.desc[1].flags &= ~VIRTQ_DESC_F_WRITE;

    // set the specified request to write to one block
    vioblk->request.type = VIRTIO_BLK_T_OUT;  // this is a write request
    vioblk->request.reserved = 0;   // no use, just set 0
    vioblk->request.sector = pos / vioblk->block_size;  // find the specified sector to write
    
    unsigned long bytes_written = 0;   // bytes we have already written
    uint8_t * buffer_ptr = (uint8_t *)buf;  // pointer to track buffer location
    uint64_t start_pos = pos % vioblk->block_size;  // starting writing position in the block

    // loop until we have write len number of bytes
    while (len > bytes_written) {
        // copy data from the given buffer to the device buffer
        unsigned long copy_size = vioblk->block_size - start_pos;
        // if the rest bytes to write is not enough
        if (copy_size > len - bytes_written) {
            copy_size = len - bytes_written;
        }
        // copy
        memcpy(&vioblk->vioblk_buf[start_pos], buffer_ptr, copy_size);


        vioblk->status = -1;    // indicate that the request has not been completed
        
        // update virtq_avail
        vioblk->vq.avail.ring[vioblk->vq.avail.idx % VIOBLK_VIRTQ_SIZE] = 0;  // qid is 0
        vioblk->vq.avail.idx++;

        // notify the device that there is a new request
        // the qid is always 0
        virtio_notify_avail(vioblk->regs, 0);
        
        // avoid race conditions
        int pie = disable_interrupts();
        // wait on the vioblk_finish condition, which means the data has been written to the device
        while (vioblk->vq.last_used_idx == vioblk->vq.used.idx) {
            condition_wait(&vioblk->vioblk_finish);
            // memory barrier function
            __sync_synchronize();
        }
        restore_interrupts(pie);

        // update the the latest index of the used virtqueue
        vioblk->vq.last_used_idx = vioblk->vq.used.idx;
        // memory barrier function
        __sync_synchronize();

        // check device return status
        if (vioblk->status != VIRTIO_BLK_S_OK) {
            lock_release(&vioblk->vioblk_lock);
            return -EIO;
        }

        // update to write to the next block
        buffer_ptr += copy_size;
        bytes_written += copy_size;
        // write to the next sector
        vioblk->request.sector++;
        // the rest of the block should be written from the beginning
        start_pos = 0;
    }

    lock_release(&vioblk->vioblk_lock);

    // return the number of bytes successfully written
    return bytes_written;    
}

/* int vioblk_cntl (struct io * io, int cmd, void * arg)
 * Inputs: struct io * io - pointer to the I/O interface of the vioblk device
 *         int cmd - command that indicates what we want information about
 *         void * arg - a generic pointer to the argument for the command
 * Outputs: return 0 if succeeds; else, return an error code
 * Description: This function does a special ioctl function corresponding to the block device.
 *              This function only supports actions for cmd = GETEND and cmd = GETBLKSZ.
 * Side Effects: None
 */
int vioblk_cntl (struct io * io, int cmd, void * arg) {
    // get the vioblk device instance
    struct vioblk_device * vioblk = (void*)io - offsetof(struct vioblk_device, io);

    // get the size of the disk in bytes
    if (cmd == IOCTL_GETEND) {
        // total disk size = number of blocks × size per block
        *(unsigned long long*) arg = vioblk->regs->config.blk.capacity * vioblk->block_size;
        return 0;
    }

    // get the block size
    else if (cmd == IOCTL_GETBLKSZ) {
        return vioblk->block_size;
    }

    // unsupported cmd values return -ENOTSUP
    else {
        return -ENOTSUP;
    }
}

/* void vioblk_isr(int srcno, void * aux)
 * Inputs: int srcno - interrupt number of the vioblk device
 *         void * aux - auxiliary pointer to pass the device in
 * Outputs: None
 * Description: This function is the handler for interrupts received from the block device.
 *              It sets or queries appropriate device registers. If appropriate, this function
 *              wakes up the threads that are awaiting responses from the device.
 * Side Effects: None
 */
void vioblk_isr(int srcno, void * aux) {
    // check whether the input is valid
    if (aux == NULL) {
        return;
    }

    // aux is the pointer to the vioblk device
    struct vioblk_device * vioblk = aux;

    // get the current interrupt status
    uint32_t status = vioblk->regs->interrupt_status;

    // memory barrier function
    __sync_synchronize();

    // if the interrupt status is not set, return directly
    if (status == 0) {
        return;
    }

    // notify the device that events causing the interrupt have been handled
    vioblk->regs->interrupt_ack = status;

    // broadcast the related condition
    condition_broadcast(&vioblk->vioblk_finish);
}
