#include "io.h"
#include "dev/uart.h"
#include "assert.h"
#include "conf.h"
#include "string.h"
#include "console.h"
#include "heap.h"
#include "error.h"
extern char _kimg_end[];

void main(void) {
    heap_init(_kimg_end, _kimg_end + 0x20000);
    console_init();  // Initialize UART console

    kprintf("----Starting memio test----\n");

    char data[1024];
    struct io* dev = create_memory_io(data, sizeof(data));
    const char* msg = "hello, memio!";
    int len = strlen(msg) + 1;

    long w = iowriteat(dev, 100, msg, len);
    assert(w == len);
    kprintf("Write success: %s\n", msg);

    char buf[32];
    long r = ioreadat(dev, 100, buf, len);
    assert(r == len);
    assert(strcmp(buf, msg) == 0);
    kprintf("Read success: %s\n", buf);

    const char* over = "1234567890abcdef"; // intend to write 20 bytes , but can only write 14
    w = iowriteat(dev, 1010, over, 20);
    assert(w == 14);
    kprintf("Out-of-bound write test passed (wrote %ld bytes)\n", w);

    r = ioreadat(dev, 1020, buf, 64); // intend to read 64 bytess, but can only read 4
    assert(r == 4);
    kprintf("Out-of-bound read test passed (read %ld bytes)\n", r);


    // -------- memio_cntl / ioctl() test --------
    unsigned long long end_size;
    int blksz;

    // 测试 GETEND
    int r1 = ioctl(dev, IOCTL_GETEND, &end_size);
    assert(r1 == 0);
    assert(end_size == 1024);
    kprintf("IOCTL_GETEND passed (size = %llu)\n", end_size);

    // 测试 GETBLKSZ
    blksz = ioctl(dev, IOCTL_GETBLKSZ, NULL);
    assert(blksz == 512);
    kprintf("IOCTL_GETBLKSZ passed (block size = %d)\n", blksz);

    // 测试 SETEND（应返回 -ENOTSUP）
    end_size = 2048;
    int r2 = ioctl(dev, IOCTL_SETEND, &end_size);
    assert(r2 == -ENOTSUP);
    kprintf("IOCTL_SETEND correctly not supported\n");


    ioclose(dev);
    kprintf("All memio tests passed.\n");
}
