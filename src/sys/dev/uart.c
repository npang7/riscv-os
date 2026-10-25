// uart.c - NS8250-compatible uart port // PN
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "uart.h"
#include "device.h"
#include "intr.h"
#include "heap.h"

#include "ioimpl.h"
#include "console.h"

#include "error.h"
#include "thread.h"
#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_NAME
#define UART_NAME "uart"
#endif

// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

struct uart_device {
    volatile struct uart_regs * regs;
    int irqno;
    int instno;

    struct io io;

    unsigned long rxovrcnt; // number of times OE was set //over run count

    struct ringbuf rxbuf;
    struct ringbuf txbuf;

    struct condition rx_cond; //add CP3
    struct condition tx_cond; 
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_open(struct io ** ioptr, void * aux);
static void uart_close(struct io * io);
static long uart_read(struct io * io, void * buf, long bufsz);
static long uart_write(struct io * io, const void * buf, long len);

static void uart_isr(int srcno, void * driver_private);

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// EXPORTED FUNCTION DEFINITIONS
// 

void uart_attach(void * mmio_base, int irqno) {
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write
    };

    struct uart_device * uart;

    uart = kcalloc(1, sizeof(struct uart_device));

    uart->regs = mmio_base;
    uart->irqno = irqno;

    ioinit0(&uart->io, &uart_iointf);

    // Check if we're trying to attach UART0, which is used for the console. It
    // had already been initialized and should not be accessed as a normal
    // device.

    if (mmio_base != (void*)UART0_MMIO_BASE) {

        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        // fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        // fence o,o ?
        uart->regs->lcr = 0; // DLAB=0

        uart->instno = register_device(UART_NAME, uart_open, uart);

    } else
        uart->instno = register_device(UART_NAME, NULL, NULL);

    condition_init(&uart->rx_cond, "uart_rx");  // add cp3
    condition_init(&uart->tx_cond, "uart_tx");  // 
}

/**
 * @brief Open the UART device and initialize its buffers and interrupts.
 *
 * @param ioptr A pointer to store the UART device's I/O structure.
 * @param aux A pointer to the UART device structure.
 *
 * @return 0 on success, -EBUSY if the device is already in use.
 *
 * @side_effects None.
 */
int uart_open(struct io ** ioptr, void * aux) {
    struct uart_device * const uart = aux;

    trace("%s()", __func__);

    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // FIXME your code goes here
    // Enable the RX interrupt
    uart->regs->ier |= IER_DRIE; 
    // Enable the TX  interrupt 
    uart->regs->ier |= IER_THREIE; 
    // Register 
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);
    *ioptr = &uart->io;
    uart->io.refcnt += 1;

    return 0;
}


/**
 * @brief Close the UART device by disabling its interrupts.
 *
 * @param io A pointer to the I/O structure associated with the UART device.
 *
 * @return None.
 *
 * @side_effects Disables all UART interrupts.
 *               Removes the UART interrupt source from the PLIC.
 */
void uart_close(struct io * io) {
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);  

    assert (iorefcnt(io) == 0);  // assert on other process

    // FIXME your code goes here
    // Disable all UART interrupts
    uart->regs->ier = 0;
    // Disable interrupt
    disable_intr_source(uart->irqno);
}




/**
 * @brief Read data from the UART receive buffer.
 *
 * @param io A pointer to the I/O structure associated with the UART device.
 * @param buf A pointer to the destination buffer where the received data will be stored.
 * @param bufsz The maximum number of bytes to read.
 *
 * @return The number of bytes read.
 *
 * @side_effects: None.
 */
long uart_read(struct io * io, void * buf, long bufsz) {
    // FIXME your code goes here
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);

    char * dst = buf;  // Destination buffer
    long count = 0;

    if (bufsz <= 0) return 0;

    int pie = disable_interrupts();
    while (rbuf_empty(&uart->rxbuf)) {
        condition_wait(&uart->rx_cond);  // cp3
    }
    restore_interrupts(pie);

    while (!rbuf_empty(&uart->rxbuf) && count < bufsz) {
        dst[count++] = rbuf_getc(&uart->rxbuf);
    }
    uart->regs->ier |= IER_DRIE;
    return count;  
}


/**
 * @brief Write data to the UART transmit buffer.
 *
 * @param io A pointer to the I/O structure associated with the UART device.
 * @param buf A pointer to the source buffer containing data to be transmitted.
 * @param len The number of bytes to write.
 *
 * @return The number of bytes written.
 *
 * @side_effects: Writes data into the UART transmit buffer.
 */

long uart_write(struct io * io, const void * buf, long len) {
    // FIXME your code goes here
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);

    char * dst = (char *)buf;  // Destination buffer
    long count = 0;

    if (len <= 0) return 0;

    int pie = disable_interrupts();
    while (rbuf_full(&uart->txbuf)) {
        condition_wait(&uart->tx_cond);  // mp2cp3
    }
    restore_interrupts(pie);

    while (!rbuf_full(&uart->txbuf) && count < len) {
        rbuf_putc(&uart->txbuf, *dst);
        dst ++;
        count ++;
    }
    uart->regs->ier |= IER_THREIE;
    return count;  // read bytes
}


/**
 * @brief UART interrupt service routine (ISR).
 *
 * @param srcno The interrupt source number (ignored in this implementation).
 * @param aux A pointer to the UART device structure.
 *
 * @return None.
 *
 * @side_effects None.
 */
void uart_isr(int srcno, void * aux) {
    // FIXME your code goes here
    struct uart_device * const uart = aux;

    // Check the UART Line Status Register
    uint8_t lsr = uart->regs->lsr;

    // If there is received data available && RX buffer is not full
    if ((lsr & LSR_DR) && !rbuf_full(&uart->rxbuf)) {
        char c = uart->regs->rbr;  
        rbuf_putc(&uart->rxbuf, c);
    }
    // If TX buffer has data && THR  is empty
    if (lsr & LSR_THRE) {
        trace("uart_isr: THR empty, sending next byte");
        if (!rbuf_empty(&uart->txbuf)) {
            uart->regs->thr = rbuf_getc(&uart->txbuf);
        }
    }
    // If RX buffer is full, disable the receive interrupt
    if (rbuf_full(&uart->rxbuf)) {
        uart->regs->ier &= ~IER_DRIE;
    }
    // If TX buffer is empty, disable the transmit interrupt
    if (rbuf_empty(&uart->txbuf)) {
        uart->regs->ier &= ~IER_THREIE;
    }
    
    if (!rbuf_empty(&uart->rxbuf)) {
        condition_broadcast(&uart->rx_cond);  // add cp3
    }
    if (!rbuf_full(&uart->txbuf)) {
        condition_broadcast(&uart->tx_cond);  // add cp3
    }
}




void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}

int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}

int rbuf_full(const struct ringbuf * rbuf) {
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
}

void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}
