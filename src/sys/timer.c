// timer.c
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}

/* void alarm_init(struct alarm * al, const char * name)
 * Inputs: struct alarm * al - the new alarm to be initialized
 *         const char * name - name of the new alarm
 * Outputs: None
 * Description: This function performs necessary initialization for the given alarm. 
 * Side Effects: None
 */
void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here

    // check whether the given alarm is valid
    if (al == NULL) {
        return;
    }

    // if no name is given, it will be simply named as "alarm"
    if (name == NULL) {
        name = "alarm";
    }

    // initialize the condition of the alarm
    condition_init(&al->cond, name);

    al->next = NULL;

    // twake is initialized to be the current tick count
    al->twake = rdtime();
}

/* void alarm_sleep(struct alarm * al, unsigned long long tcnt)
 * Inputs: struct alarm * al - the alarm object that the thread will wait for a wake-up event on
 *         unsigned long long tcnt - the number of ticks to put the thread to sleep for relative to twake
 * Outputs: None
 * Description: This function puts the current thread to sleep until the appropriate time in ticks at which
 *              the sleep duration ends.
 * Side Effects: The value in the mtimecmp register may be changed.
 */
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    //struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        // update twake for next wake-up
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;

    // FIXME your code goes here

    // disable the interrupts to avoid race conditions
    pie = disable_interrupts();


    // find the proper position to add this alarm to sleep_list

    // if the sleep_list is empty, simply set the alarm to be the head of the list
    if (sleep_list == NULL) {
        sleep_list = al;
        // set the new interrupt expiry time
        set_stcmp(al->twake);
    }

    // if the sleep_list is not empty
    else {
        // only when twake of the given alarm is the earliest, we need to update mtimecmp
        if (sleep_list->twake > al->twake) {
            // set the new interrupt expiry time
            set_stcmp(al->twake);

            // set the alarm to be the head of the sleep_list
            al->next = sleep_list;
            sleep_list = al;
        }

        // if twake is not the earliest, no need to update mtimecmp
        else {
            // loop through the sleep_list to find the proper position
            struct alarm * alm = sleep_list;
            while (alm->next != NULL && alm->next->twake <= al->twake) {
                alm = alm->next;
            }

            // add this alarm to the proper position in the sleep_list
            al->next = alm->next;
            alm->next = al;
        }
    }


    // enable timer interrupts
    csrs_sie(RISCV_SIE_STIE);

    // put current thread to sleep
    condition_wait(&al->cond);

    // restore the interrupts
    restore_interrupts(pie);
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

// handle_timer_interrupt() is dispatched from intr_handler in intr.c

/* void handle_timer_interrupt(void)
 * Inputs: None
 * Outputs: None
 * Description: This function is an interrupt service routine. It handles timer interrupts by waking
 *              all ready threads and updating the sleep_list linked list and mtimecmp registers accordingly.
 * Side Effects: The value in the mtimecmp register may be changed.
 */
void handle_timer_interrupt(void) {
    //struct alarm * head = sleep_list;
    //struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // FIXME your code goes here

    // remove all alarms that are past their threshold wake-up event time from the sleep_list
    struct alarm * alm;
    while (sleep_list != NULL) {
        if (now >= sleep_list->twake) {
            alm = sleep_list;
            sleep_list = sleep_list->next;

            // wake all threads waiting on this alarm condition
            condition_broadcast(&alm->cond);

            alm->next = NULL;
        }
        else {
            break;
        }
    }

    // if the sleep_list is not empty
    if (sleep_list != NULL) {
        // set the timer interrupt threshold to waiting for the next wake-up event on the sleep_list
        set_stcmp(sleep_list->twake);
    }
    // if the sleep_list is empty
    else {
        // disable timer interrupts
        csrc_sie(RISCV_SIE_STIE);
    }
}
