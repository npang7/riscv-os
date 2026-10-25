#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 

#define INIT_NAME "shell.elf"
#define NUM_UARTS 3


void main(void) {
    struct io *blkio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();


    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < NUM_UARTS; i++) // change for number of UARTs
        uart_attach((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO+i);
        
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }
    enable_interrupts();

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }


    // insert testcase below
    // This test case will run fib and trek simultaneously. 
    struct io *trekFibio;
    result = fsopen(INIT_NAME, &trekFibio);
    if (result < 0) {
        kprintf(INIT_NAME ": %s; Unable to open\n");
        panic("Failed to open trekfib\n");
    }
    result = process_exec(trekFibio, 0, NULL);
    
}











/*#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 




void main(void) {
    struct io *blkio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();


    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }

    result = open_device("uart", 1, &current_process()->iotab[2]);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open UART\n");
    }*/

    // insert testcase below
    // This test case will pass in PrintingString through argv and have it print out to the console. has to be used in conjunction with 
    // the sysArgPrint_test user program, or something similar, for correct output. 
    /*struct io *sysArgPrintio;
    char PrintingString[] = "\n Asynchronous Grade: Passing \n"; 
    char *argv[2] = {0}; 

    argv[0] = PrintingString; 
    argv[1] = NULL; 
    result = fsopen("sysArg_test", &sysArgPrintio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open syswait_test\n");
    }
    result = process_exec(sysArgPrintio, 1, (char **)argv);*/
    /*char *argv[2] = {0}; 

    argv[0] = "dsave.dat"; 
    argv[1] = NULL;*/

    /*struct io * testio;
    result = fsopen("shell.elf", &testio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open testfork\n");
    }
    result = process_exec(testio, 0, NULL);
}*/

// main for test
/*void main(void) {
    struct io *blkio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();


    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }

    struct io * testio;
    result = fsopen("test", &testio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open test\n");
    }
    result = process_exec(testio, 0, NULL);
}*/