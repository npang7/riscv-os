// io.c - Unified I/O object
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include "thread.h"
#include "memory.h"

#include <stddef.h>
#include <limits.h>
// INTERNAL TYPE DEFINITIONS
//

struct memio {
    struct io io; // I/O struct of memory I/O
    void * buf; // Block of memory
    size_t size; // Size of memory block
};

// define PIPE_BUFSZ PAGE_SIZE
typedef struct pipe {
    struct io io_read;
    struct io io_write;

    char *buffer;
    int head;
    int tail;
    int closed_read;
    int closed_write;

    struct condition *cond_read;
    struct condition *cond_write;
    struct lock *lock;
} pipe_t;

struct seekio {
    struct io io; // I/O struct of seek I/O
    struct io * bkgio; // Backing I/O supporting _readat_ and _writeat_
    unsigned long long pos; // Current position within backing endpoint
    unsigned long long end; // End position in backing endpoint
    int blksz; // Block size of backing endpoint
};

// INTERNAL FUNCTION DEFINITIONS
//

static int memio_cntl(struct io * io, int cmd, void * arg);

static long memio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long memio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);

static void seekio_close(struct io * io);

static int seekio_cntl(struct io * io, int cmd, void * arg);

static long seekio_read(struct io * io, void * buf, long bufsz);

static long seekio_write(struct io * io, const void * buf, long len);

static long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);


// INTERNAL GLOBAL CONSTANTS
static const struct iointf seekio_iointf = {
    .close = &seekio_close,
    .cntl = &seekio_cntl,
    .read = &seekio_read,
    .write = &seekio_write,
    .readat = &seekio_readat,
    .writeat = &seekio_writeat
};


// EXPORTED FUNCTION DEFINITIONS
//

struct io * ioinit0(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    assert (intf != NULL);
    io->intf = intf;
    io->refcnt = 0;
    return io;
}

struct io * ioinit1(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    io->intf = intf;
    io->refcnt = 1;
    return io;
}

unsigned long iorefcnt(const struct io * io) {
    assert (io != NULL);
    return io->refcnt;
}

struct io * ioaddref(struct io * io) {
    assert (io != NULL);
    io->refcnt += 1;
    return io;
}

void ioclose(struct io * io) {
    assert (io != NULL);
    assert (io->intf != NULL);
    
    assert (io->refcnt != 0);
    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io * io, void * buf, long bufsz) {
    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->read(io, buf, bufsz);
}

long iofill(struct io * io, void * buf, long bufsz) {
	long bufpos = 0; // position in buffer for next read
    long nread; // result of last read

    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    while (bufpos < bufsz) {
        nread = io->intf->read(io, buf+bufpos, bufsz-bufpos);
        
        if (nread <= 0)
            return (nread < 0) ? nread : bufpos;
        
        bufpos += nread;
    }

    return bufpos;
}

long iowrite(struct io * io, const void * buf, long len) {
	long bufpos = 0; // position in buffer for next write
    long n; // result of last write

    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;
    
    do {
        n = io->intf->write(io, buf+bufpos, len-bufpos);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->readat == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->writeat == NULL)
        return -ENOTSUP;
    
    if (len < 0)
        return -EINVAL;
    
    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io * io, int cmd, void * arg) {
    assert (io != NULL);
    assert (io->intf != NULL);

	if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioblksz(struct io * io) {
    return ioctl(io, IOCTL_GETBLKSZ, NULL);
}

int ioseek(struct io * io, unsigned long long pos) {
    return ioctl(io, IOCTL_SETPOS, &pos);
}

//fix by me
static const struct iointf memio_iointf = {
    .close = NULL,
    .cntl = memio_cntl,
    .read = NULL,
    .write = NULL,
    .readat = memio_readat,
    .writeat = memio_writeat
};

/**
 * create_memory_io
 *
 * Description:
 *     This function creates a memory-backed I/O interface, which allows reading
 *     and writing to a user-provided buffer.
 *
 * Parameters:
 *     @buf: Pointer to a memory buffer that backs the I/O object
 *     @size: The size of the memory buffer
 *
 * Returns:
 *     A pointer to a newly allocated "struct io" object that operates on the
 *     given buffer. Returns NULL if memory allocation fails.
 *
 * Side Effects:
 *     None.
 */
struct io * create_memory_io(void * buf, size_t size) {
    // FIX ME
    struct memio * mio = kmalloc(sizeof(struct memio));  //###kcalloc?kmalloc?
    mio->buf = buf;
    mio->size = size; 
    //mio->io.intf = &memio_iointf; 
    return ioinit1(&mio->io, &memio_iointf);
}


struct io * create_seekable_io(struct io * io) {
    struct seekio * sio;
    unsigned long end;
    int result;
    int blksz;
    
    blksz = ioblksz(io);
    assert (0 < blksz);
    
    // block size must be power of two
    assert ((blksz & (blksz - 1)) == 0);

    result = ioctl(io, IOCTL_GETEND, &end);
    assert (result == 0);
    
    sio = kcalloc(1, sizeof(struct seekio));

    sio->pos = 0;
    sio->end = end;
    sio->blksz = blksz;
    sio->bkgio = ioaddref(io);

    return ioinit1(&sio->io, &seekio_iointf);

};

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * memio_readat
 *
 * Description:
 *     Reads up to "bufsz" bytes from the memory-backed I/O device starting
 *     at logical position "pos" and stores the result in "buf".
 *
 * Parameters:
 *     @io:     Pointer to the generic I/O object
 *     @pos:    Offset into the memory buffer to start reading from
 *     @buf:    Pointer to the destination buffer to copy data into
 *     @bufsz:  Maximum number of bytes to read
 *
 * Returns:
 *     Number of bytes read.
 *
 * Side Effects:
 *      Copies data from the internal memory buffer to "buf"
 */
long memio_readat (
    struct io * io,
    unsigned long long pos,
    void * buf, long bufsz)
{
    // FIX ME
    struct memio * mio = (void*)io - offsetof(struct memio, io);

    if (pos >= mio->size)
        return 0; // beyond edge

    if (pos + bufsz > mio->size)
        bufsz = mio->size - pos; // update length

    memcpy(buf, mio->buf + pos, bufsz);
    return bufsz;
}

/**
 * memio_writeat
 *
 * Description:
 *     Writes up to "len" bytes from "buf" into the memory-backed device,
 *     starting at logical position "pos". 
 *
 * Parameters:
 *     @io:   Pointer to the generic I/O object (casted internally to memio)
 *     @pos:  Offset (in bytes) into the memory buffer where data will be written
 *     @buf:  Pointer to the source buffer containing the data to write
 *     @len:  Number of bytes to write
 *
 * Returns:
 *     Number of bytes written.
 *
 * Side Effects:
 *    Modifies the internal memory buffer at offset "pos"
 */
long memio_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf, long len)
{
    // FIX ME
    struct memio * mio = (void*)io - offsetof(struct memio, io);

    if (pos >= mio->size)
        return 0; // beyond edge

    if (pos + len > mio->size)
        len = mio->size - pos; // update length

    memcpy(mio->buf + pos, buf, len);
    return len;
}

/**
 * memio_cntl 
 *
 * Description:
 *     This function processes control commands for the memory I/O
 *     interface. It supports querying the block size and total size, but does not
 *     support resizing the buffer.
 *
 * Parameters:
 *     @io:   Pointer to the I/O interface object
 *     @cmd:  Control command to be executed. Supported commands:
 *            IOCTL_GETBLKSZ: get block size (returns 512)
 *            IOCTL_GETEND: get total size in bytes (writes to *arg)
 *            IOCTL_SETEND: set new end (return 0 success)
 *     @arg:  Argument for the command (used only for GETEND)
 *
 * Returns:
 *     - For IOCTL_GETBLKSZ: returns 512
 *     - For IOCTL_GETEND: returns 0 and sets *(unsigned long long*)arg = size
 *     - For unsupported commands: returns -ENOTSUP
 */
int memio_cntl(struct io * io, int cmd, void * arg) {
    // FIX ME
    struct memio * mio = (void*)io - offsetof(struct memio, io);

    switch (cmd) {
        case IOCTL_GETBLKSZ:
            return 1; // fix 1 byte
        case IOCTL_GETEND:
            *(unsigned long long*)arg = mio->size;
            return 0;
        case IOCTL_SETEND:
            if (!arg) return -EINVAL;
            size_t new_size = *(size_t *)arg;
            if (new_size > mio->size) {
                return -EINVAL;}
            mio->size = new_size;
            return 0;
        default:
            return -ENOTSUP;
    }
}

void seekio_close(struct io * io) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    sio->bkgio->refcnt--;
    ioclose(sio->bkgio);
    kfree(sio);
}

int seekio_cntl(struct io * io, int cmd, void * arg) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long * ullarg = arg;
    int result;

    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return sio->blksz;
    case IOCTL_GETPOS:
        *ullarg = sio->pos;
        return 0;
    case IOCTL_SETPOS:
        // New position must be multiple of block size
        if ((*ullarg & (sio->blksz - 1)) != 0)
            return -EINVAL;
        
        // New position must not be past end
        if (*ullarg > sio->end)
            return -EINVAL;
        
        sio->pos = *ullarg;
        return 0;
    case IOCTL_GETEND:
        *ullarg = sio->end;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        result = ioctl(sio->bkgio, IOCTL_SETEND, ullarg);
        if (result == 0)
            sio->end = *ullarg;
        return result;
    default:
        return ioctl(sio->bkgio, cmd, arg);
    }
}

long seekio_read(struct io * io, void * buf, long bufsz) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long const end = sio->end;
    long rcnt;

    // Cannot read past end
    if (end - pos < bufsz)
        bufsz = end - pos;

    if (bufsz == 0)
        return 0;
        
    // Request must be for at least blksz bytes if not zero
    if (bufsz < sio->blksz)
        return -EINVAL;

    // Truncate buffer size to multiple of blksz
    bufsz &= ~(sio->blksz - 1);

    rcnt = ioreadat(sio->bkgio, pos, buf, bufsz);
    sio->pos = pos + ((rcnt < 0) ? 0 : rcnt);
    return rcnt;
}


long seekio_write(struct io * io, const void * buf, long len) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long end = sio->end;
    int result;
    long wcnt;

    if (len == 0)
        return 0;
    
    // Request must be for at least blksz bytes
    if (len < sio->blksz)
        return -EINVAL;
    
    // Truncate length to multiple of blksz
    len &= ~(sio->blksz - 1);

    // Check if write is past end. If it is, we need to change end position.

    if (end - pos < len) {
        if (ULLONG_MAX - pos < len)
            return -EINVAL;
        
        end = pos + len;

        result = ioctl(sio->bkgio, IOCTL_SETEND, &end);
        
        if (result != 0)
            return result;
        
        sio->end = end;
    }

    wcnt = iowriteat(sio->bkgio, sio->pos, buf, len);
    sio->pos = pos + ((wcnt < 0) ? 0 : wcnt);
    return wcnt;
}

long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return ioreadat(sio->bkgio, pos, buf, bufsz);
}

long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return iowriteat(sio->bkgio, pos, buf, len);
}




/**
 * pipe_read
 *
 * Reads up to bufsz bytes from a pipe into buf.
 * Blocks if the pipe is empty and write end is open.
 *
 * @io:    I/O object for the pipe
 * @buf:   Destination buffer
 * @bufsz: Max number of bytes to read
 *
 * @return Number of bytes read, 0 if fail
 */
static long pipe_read(struct io *io, void *buf, long bufsz) {
    pipe_t *p = (void*)io - offsetof(pipe_t, io_read);
    long count = 0;

    lock_acquire(p->lock);
    while (count < bufsz) {
        while (p->head == p->tail) {     // Wait if pipe is empty
            if (p->closed_write) {
                lock_release(p->lock);
                return count; 
            }
            condition_wait(p->cond_read);
        }
        ((char*)buf)[count++] = p->buffer[p->head++ % PIPE_BUF];
        condition_broadcast(p->cond_write);//Wake up writers
    }
    lock_release(p->lock);
    return count;
}



/**
 * pipe_write
 *
 * Writes up to len bytes from buf into the pipe.
 * Blocks if the pipe is full and the read end is open.
 *
 * @io:  I/O object for the pipe
 * @buf: Source buffer
 * @len: Number of bytes to write
 *
 * @return Number of bytes written, or -EPIPE if read end is closed
 */
static long pipe_write(struct io *io, const void *buf, long len) {
    kprintf("[WRITER] I'm alive!\n");

    pipe_t *p = (void*)io - offsetof(pipe_t, io_write);
    long count = 0;

    lock_acquire(p->lock);
    while (count < len) {
        while ((p->tail + 1) % PIPE_BUF == p->head % PIPE_BUF) { // Wait if pipe is full
            if (p->closed_read) {
                lock_release(p->lock);
                return -EPIPE;
            }
            condition_wait(p->cond_write);
        }
        p->buffer[p->tail++ % PIPE_BUF] = ((char*)buf)[count++];
        condition_broadcast(p->cond_read); // Wake up readers
    }
    lock_release(p->lock);
    return count;
}



/**
 * pipe_close_read
 *
 * Closes the read end of the pipe.
 * If both ends are closed, release resource.
 *
 * @io: I/O object for reading
 */
static void pipe_close_read(struct io *io) {
    pipe_t *p = (void*)io - offsetof(pipe_t, io_read);
    lock_acquire(p->lock);
    p->closed_read = 1;
    condition_broadcast(p->cond_write);
    lock_release(p->lock);
    if (p->closed_write) { // Free pipe if both ends are closed
        free_phys_page(p->buffer);
        kfree(p);
    }
}


/**
 * pipe_close_write
 *
 * Closes the write end of the pipe.
 * If both ends are closed, releases resources.
 *
 * @io: I/O object for writing
 */
static void pipe_close_write(struct io *io) {
    pipe_t *p = (void*)io - offsetof(pipe_t, io_write);
    lock_acquire(p->lock);
    p->closed_write = 1;
    condition_broadcast(p->cond_read);
    lock_release(p->lock);
    if (p->closed_read) { // Free pipe if both ends are closed
        free_phys_page(p->buffer);
        kfree(p);
    }
}

//structure: pipe read io interface
static const struct iointf pipe_read_intf = {
    .close = pipe_close_read,
    .cntl = NULL,
    .read = pipe_read,
    .write = NULL,
    .readat = NULL,
    .writeat = NULL
};
//structure: pipe write io interface
static const struct iointf pipe_write_intf = {
    .close = pipe_close_write,
    .cntl = NULL,
    .read = NULL,
    .write = pipe_write,
    .readat = NULL,
    .writeat = NULL
};


/**
 * create_pipe
 *
 * Initializes a new pipe with separate read/write I/O interfaces.
 *
 * @wioptr: Output pointer to write-end I/O interface
 * @rioptr: Output pointer to read-end I/O interface
 *
 * Side Effects:
 *     Allocates memory for the pipe, lock, condition variables, and buffer
 */
void create_pipe(struct io **wioptr, struct io **rioptr) {
    pipe_t *p = kcalloc(1, sizeof(pipe_t));
    p->buffer = (char *)alloc_phys_page();
    p->head = p->tail = 0;
    p->closed_read = p->closed_write = 0;
    // Initialize lock
    p->lock = kmalloc(sizeof(struct lock));
    lock_init(p->lock);
    // Initialize condition
    p->cond_read = kmalloc(sizeof(struct condition));
    condition_init(p->cond_read, "pipe_read");

    p->cond_write = kmalloc(sizeof(struct condition));
    condition_init(p->cond_write, "pipe_write");

    ioinit1(&p->io_read, &pipe_read_intf); // Initialize I/O intf
    ioinit1(&p->io_write, &pipe_write_intf);

    *rioptr = &p->io_read;
    *wioptr = &p->io_write;
}