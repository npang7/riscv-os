#include "fs.h"
#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "console.h"
#include "device.h"
#include "conf.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "heap.h"
#include "process.h"
#include "thread.h"
#include "memory.h"
#include "intr.h"
#include "fs.h"
#include "io.h"
#include "assert.h"
#include "string.h"
#include "console.h"
#include "device.h"
#include "conf.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "heap.h"
#include "process.h"
#include "thread.h"
#include "memory.h"
#include "intr.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE - VIRTIO0_MMIO_BASE)
#define KTFS_BLKSZ 512

extern char _kimg_end[];

void main(void) {

    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();

    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO + 0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO + 1);
    rtc_attach((void*)RTC_MMIO_BASE);

    for (int i = 0; i < 8; i++) {
        virtio_attach((void*)VIRTIO0_MMIO_BASE + i * VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    struct io *blkio;
    assert(open_device("vioblk", 0, &blkio) == 0);
    assert(fsmount(blkio) == 0);
    
    kprintf("\nktfs tests\n");

    const char *filename = "ktfs-test";
    const char *msg = "ECE391-KTFS-Test";
    char read_buf[64] = {0};
    unsigned long long new_size = strlen(msg);
    long wlen, rlen;
    struct io *io;

    // clean up leftover file
    fsdelete(filename);

    // create 
    kprintf("\nCreate\n");
    assert(fscreate(filename) == 0);

    // create again
    assert(fscreate(filename) < 0);

    // Open and Extend
    assert(fsopen(filename, &io) == 0);
    assert(io->intf->cntl(io, IOCTL_SETEND, &new_size) == 0);
    kprintf("%u bytes\n", new_size);

    // shrinking
    unsigned long long shrink = new_size - 1;
    assert(io->intf->cntl(io, IOCTL_SETEND, &shrink) < 0);

    // expand across multiple blocks
    unsigned long long large_size = 3 * KTFS_BLKSZ + 512;
    assert(io->intf->cntl(io, IOCTL_SETEND, &large_size) == 0);
    kprintf("%u bytes\n", large_size);

    // write and Edge Write 
    wlen = io->intf->writeat(io, 0, msg, strlen(msg));
    assert(wlen == (long)strlen(msg));
    kprintf("%ld bytes\n", wlen);

    // write beyond end of file
    wlen = io->intf->writeat(io, large_size + 10, msg, strlen(msg));
    assert(wlen == 0);

    // truncated write near end
    long partial = io->intf->writeat(io, large_size - 4, msg, 10);
    assert(partial == 4);
    kprintf("%ld bytes\n", partial);

    // zero-length write
    long zero = io->intf->writeat(io, 0, msg, 0);
    assert(zero == 0);

    // read 
    memset(read_buf, 0, sizeof(read_buf));
    rlen = io->intf->readat(io, 0, read_buf, strlen(msg));
    assert(rlen == (long)strlen(msg));
    assert(memcmp(read_buf, msg, rlen) == 0);
    read_buf[rlen] = '\0';
    kprintf("%s\n",  read_buf);

    // read beyond file end
    memset(read_buf, 0, sizeof(read_buf));
    rlen = io->intf->readat(io, large_size + 20, read_buf, 10);
    assert(rlen == 0);

    io->intf->close(io);

    // delete 
    assert(fsdelete(filename) == 0);

    // delete again, should fail
    assert(fsdelete(filename) < 0);
    kprintf("\nall passed\n");

}
