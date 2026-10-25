// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "error.h"
#include "console.h"
#include "process.h" //added by cp3
#include <stdarg.h>

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

#ifndef STACK_SIZE
#define STACK_SIZE 4000
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    uint64_t s[12];
    void * ra;
    void * sp;
};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};

struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;

    struct lock_list_elem * lock_list;  // add for mp3cp1
    struct process * proc; // add mp3cp2
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// the lock API functions
void lock_init(struct lock * lock);
void lock_acquire(struct lock * lock);
void lock_release(struct lock * lock);

// helper functions for lock_list design
void lock_list_insert(struct thread * thr, struct lock * lock);
void lock_list_remove(struct thread * thr, struct lock * lock);
void release_all_locks(struct thread * thr);


// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1)__attribute__((unused));  // avoid warnings

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_RUNNING,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    // FIXME your code goes here

    // I put the entry function in s8 in thread_spawn, so initialize idle_thread in the same manner
    .ctx.s[8] = (unsigned long) &idle_thread_func
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//

int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

/* int thread_spawn(const char * name, void (*entry)(void), ...)
 * Inputs: const char * name - the name of the new thread
 *         void (*entry)(void) - pointer to the function that the thread will start executing
 *         arguments after entry (up to eight) - arguments to be passed to the entry function when it is executed
 * Outputs: return the thread id of the new thread
 * Description: This function creates and schedules a new thread. It ensures that when the new thread is scheduled
 *              to execute, it will jump to the entry function provided, and the additional arguments provided will
 *              be passed to the entry function. In addition, when entry finishes, thread_exit is called.
 * Side Effects: New memory is allocated for the new thread.
 */
int thread_spawn (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    // create a new thread
    child = create_thread(name);

    // the maximum number of threads has been reached
    // no new threads can be created
    if (child == NULL)
        return -EMTHR;

    // set the thread state
    set_thread_state(child, THREAD_READY);

    // add the newly created thread to the ready list
    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

    // FIXME your code goes here
    // filling in entry function arguments is given below, the rest is up to you
    // save the optional arguments in the part of the thread context structure normally used to save registers s0 through s7
    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);


    // put the entry function in s8
    child->ctx.s[8] = (uint64_t) entry;

    // after _thread_swtch, the new thread will first get into _thread_startup
    // in _thread_startup, the new thread will get into the entry function with provided arguments, and finish with thread_exit
    child->ctx.ra = (void *) &_thread_startup;
    // set the stack base for the new thread
    child->ctx.sp = (void *) child->stack_anchor;


    return child->id;
}

/* void thread_exit(void)
 * Inputs: None
 * Outputs: None
 * Description: This function terminates the currently running thread. This function does not return.
 * Side Effects: None
 */
void thread_exit(void) {
    // FIXME your code goes here

    // if the currently running thread is the main thread, call halt_success()
    if (TP == &main_thread) {
        // clear the lock_list
        release_all_locks(TP);

        halt_success();
    }
    
    else {
        // else, set the current thread’s state to THREAD_EXITED
        set_thread_state(TP, THREAD_EXITED);

        // clear the lock_list
        release_all_locks(TP);

        // signal the parent thread in case it is waiting for the current thread to exit
        if (TP->parent != NULL) {
            condition_broadcast(&TP->parent->child_exit);
        }

        // suspend this thread, which should not return
        running_thread_suspend();

        // if we somehow manage to return
        halt_failure();
    }
}

void thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}

/* int thread_join(int tid)
 * Inputs: int tid - the thread id of the child thread to wait for
 * Outputs: return the thread id of the child thread that exited, or an error in some cases
 * Description: This function waits for the identified child of the running thread to exit, if tid is not zero.
 *              Otherwise, the function waits for any child of the running thread. If the child has already exited,
 *              thread_join doesn't wait and return immediately. If the child is still running, the parent should wait
 *              on the condition variable child_exit in its own struct thread.
 * Side Effects: The parent releases the resources used by the child thread.
 */
int thread_join(int tid) {
    // FIXME your code goes here

    int pie;

    // if tid is not zero (the running thread is not the main thread)
    if (tid != 0) {

        // return -EINVAL if the identified thread does not exist or is not a child of the running thread
        if (thrtab[tid] == NULL || thrtab[tid]->parent != TP) {
            return -EINVAL;
        }

        
        // wait for the identified child of the running thread to exit

        // if child already exited
        if (thrtab[tid]->state == THREAD_EXITED) {
            // release the resources used by the child thread
            thread_reclaim(tid);
            // return immediately
            return tid;
        }

        // if child still running
        else {
            pie = disable_interrupts();
            // wait on the condition variable child_exit
            while (thrtab[tid]->state != THREAD_EXITED) {
                condition_wait(&TP->child_exit);
            }
            restore_interrupts(pie);

            // release the resources used by the child thread
            thread_reclaim(tid);

            return tid;
        }
    }



    // if tid is zero (the running thread is the main thread)
    else {

        // wait for any child of the running thread

        // first find a child of the currently runnning thread
        int num = 0;    // record the number of children of the running thread
        int thr_id = 1;
        int i;
        // the array to record all the children's tid of the running thread
        int children[NTHR];
        // initialize all members to be -1
        for (i = 0; i < NTHR; i++) {
            children[i] = -1;
        }

        // count the number of children of the running thread
        while (thr_id < NTHR) {
            if (thrtab[thr_id] != NULL) {
                if (thrtab[thr_id]->parent == TP) {
                    num += 1;
                    // record this child thread
                    children[num-1] = thr_id;
                }
            }
            thr_id += 1;
        }
        // return -EINVAL if the running thread does not have any children
        if (num == 0) {
            return -EINVAL;
        }

        // if the running thread has children, find if one of its children is already EXITED
        i = 0;
        while (children[i] != -1) {
            if (thrtab[children[i]]->state == THREAD_EXITED) {
                // release the resources used by the child thread
                thread_reclaim(children[i]);
                // return immediately
                return children[i];         
            }
            i++;
        }

        pie = disable_interrupts();
        // if all children are still running, wait on the condition variable child_exit
        condition_wait(&TP->child_exit);
        restore_interrupts(pie);

        // find the child thread which is EXITED
        i = 0;
        while (children[i] != -1) {
            if (thrtab[children[i]]->state == THREAD_EXITED) {
                // release the resources used by the child thread
                thread_reclaim(children[i]);

                return children[i];         
            }
            i++;
        }
    }

    return -1;  // avoid warnings
}

const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition * cond) {
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_RUNNING);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}

/* void condition_broadcast(struct condition * cond)
 * Inputs: struct condition * cond - the condition variable that the threads are waiting
 * Outputs: None
 * Description: This function wakes up threads waiting on the condition variable by first changing its
 *              state from THREAD_WAITING to THREAD_READY, placing it on the ready list, and removing it
 *              from the list of threads waiting on the condition.
 * Side Effects: It changes the members of cond->wait_list and ready_list.
 */
void condition_broadcast(struct condition * cond) {
    // FIXME your code goes here

    struct thread * thr;
    int pie;

    // loop through the waiting_list
    while (!tlempty(&cond->wait_list)) {
        // remove the thread from the wait_list
        pie = disable_interrupts();
        thr = tlremove(&cond->wait_list);
        restore_interrupts(pie);

        // change the thread's state from THREAD_WAITING to THREAD_READY
        set_thread_state(thr, THREAD_READY);

        // place the thread on the ready_list
        pie = disable_interrupts();
        tlinsert(&ready_list, thr);
        restore_interrupts(pie);
    }
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_RUNNING] = "RUNNING",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // clear the lock_list
    release_all_locks(thr);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_page;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    
    stack_page = alloc_phys_page();  // changed by cp3
    anchor = stack_page + PAGE_SIZE;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    thr->lock_list = NULL;
    return thr;
}

/* void running_thread_suspend(void)
 * Inputs: None
 * Outputs: None
 * Description: This function suspends the currently running thread and resumes the next thread on the ready_list.
 *              It implements a simple round-robin scheduler. The thread being suspended, if it is still runnable,
 *              is inserted at the tail of ready_list, and the next thread to run is taken from the head of the list.
 *              Only when the calling thread is RUNNING, this function places the current thread back on the ready_list
 *              and update its state accordingly. If the calling thread is EXITED, this function frees its stack.
 * Side Effects: It changes the members of the ready_list.
 */
void running_thread_suspend(void) {
    // FIXME your code goes here

    int pie;

    // this function must be called with interrupts enabled
    enable_interrupts();

    // check whether the calling thread is in THREAD_RUNNING state
    if (TP->state == THREAD_RUNNING) {
        // update its state to THREAD_READY
        set_thread_state(TP, THREAD_READY);

        // place the current thread back on the ready_list
        pie = disable_interrupts();
        tlinsert(&ready_list, TP);
        restore_interrupts(pie);
    }

    // take next thread from the head of the ready_list and resume it
    struct thread *next_thr;
    // if the ready_list is empty
    if (tlempty(&ready_list)) {
        // set next thread to be the idle_thread
        next_thr = &idle_thread;
    }
    else {
        // else, get the next thread to run from the head of the ready_list
        pie = disable_interrupts();
        next_thr = tlremove(&ready_list);
        restore_interrupts(pie);
        
    }

    // update the next thread's state to THREAD_RUNNING
    set_thread_state(next_thr, THREAD_RUNNING);

    if (next_thr->proc != NULL) {
        switch_mspace(next_thr->proc->mtag);   //added by cp3
    }

    // switch to the next thread
    // _thread_swtch returns the original thread before switching
    struct thread * ori_thr = _thread_swtch(next_thr);

    // check whether the original thread before switching is in THREAD_EXITED state
    enum thread_state ori_state = ori_thr->state;
    if (ori_state == THREAD_EXITED) {
        // clean up (free the stack of the original thread)
        if (ori_thr->stack_lowest != NULL) {
            //kfree(ori_thr->stack_lowest);
            free_phys_page(ori_thr->stack_lowest);
            ori_thr->stack_lowest = NULL;
        }
    }
}

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

// Appends elements of l1 to the end of l0 and clears l1.

void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}




/* add the lock API for mp3cp1 */

/* void lock_init(struct lock * lock)
 * Inputs: struct lock * lock - the lock to be initialized
 * Outputs: None
 * Description: This function initializes the lock as well as the condition struct within the lock.
 * Side Effects: None
 */
void lock_init(struct lock * lock) {
    // at first, no thread holds the lock
    lock->owner = NULL;

    // initialize the condition struct within the lock
    condition_init(&lock->released, "lock_released");

    lock->count = 0;
}

/* void lock_acquire(struct lock * lock)
 * Inputs: struct lock * lock - the lock to be acquired
 * Outputs: None
 * Description: This function acquires a lock. If this lock's owner is the current running thread, increment
 *              the count of the lock. Otherwise, wait until the lock is released and then acquire the lock.
 * Side Effects: None
 */
void lock_acquire(struct lock * lock) {
    // avoid race conditions
    int pie = disable_interrupts();

    // if this lock's owner is the current running thread
    if (lock->owner == TP) {
        // increment the count of the lock
        lock->count++;

        // restore the previous interrupt state
        restore_interrupts(pie);
        return;
    }

    // otherwise, wait until the lock is released and then acquire the lock
    while (lock->owner != NULL) {
        condition_wait(&lock->released);
    }
    lock->owner = TP;
    lock->count++;

    // insert this lock into the lock_list
    lock_list_insert(TP, lock);

    // restore the previous interrupt state
    restore_interrupts(pie);
}

/* void lock_release(struct lock * lock)
 * Inputs: struct lock * lock - the lock to be released
 * Outputs: None
 * Description: This function release a lock if the current thread is the owner, adjusting count accordingly. It makes
 *              sure that if the current thread is done using the lock, it signals that to other threads waiting on the
 *              lock and releases the lock.
 * Side Effects: None
 */
void lock_release(struct lock * lock) {
    // avoid race conditions
    int pie = disable_interrupts();

    // the current thread must be owner
    assert(lock->owner == TP);

    // decrement the count of the lock
    lock->count--;

    // make sure that the current thread is done using the lock
    if (lock->count == 0) {
        // release the lock
        lock->owner = NULL;

        // remove this lock from the lock_list
        lock_list_remove(TP, lock);

        // signal that to other threads waiting on the lock
        condition_broadcast(&lock->released);
    }

    // restore the previous interrupt state
    restore_interrupts(pie);
}


/* helper functions for lock_list */

/* void lock_list_insert(struct thread * thr, struct lock * lock)
 * Inputs: struct thread * thr - the thread holding this lock
 *         struct lock * lock - the lock to be inserted into the lock_list
 * Outputs: None
 * Description: This function inserts a new lock into the lock_list.
 * Side Effects: New memory is allocated for a new lock_list node.
 */
void lock_list_insert(struct thread * thr, struct lock * lock) {
    // allocate memory for a new node in lock_list
    struct lock_list_elem * node = kmalloc(sizeof(struct lock_list_elem));
    // if the allocation fails, panic
    if (node == NULL) {
        panic("no memory for a new lock_list node");
    }

    // initialize the new node
    node->lock = lock;

    // insert the node into lock_list
    node->next = thr->lock_list;
    thr->lock_list = node;
}

/* void lock_list_remove(struct thread * thr, struct lock * lock)
 * Inputs: struct thread * thr - the thread holding this lock
 *         struct lock * lock - the lock to be removed from the lock_list
 * Outputs: None
 * Description: This function removes a lock from the lock_list.
 * Side Effects: The memory for the removed lock_list node is freed.
 */
void lock_list_remove(struct thread * thr, struct lock * lock) {
    struct lock_list_elem ** current_node = &thr->lock_list;

    // loop to find whether the lock_list contains the specified lock
    while (*current_node != NULL) {
        // if this lock is found
        if ((*current_node)->lock == lock) {
            // remove and free this node
            struct lock_list_elem * free = *current_node;
            *current_node = (*current_node)->next;
            kfree(free);
            return;
        }

        // go to the next node
        current_node = &(*current_node)->next;
    }
}

/* void release_all_locks(struct thread * thr)
 * Inputs: struct thread * thr - the exiting thread to be released all locks
 * Outputs: None
 * Description: This function releases all locks in the lock_list of the current thread.
 * Side Effects: The memory for the released lock_list node is freed.
 */
void release_all_locks(struct thread * thr) {
    // loop to release all locks in the lock_list
    while (thr->lock_list != NULL) {
        struct lock * lock = thr->lock_list->lock;
        lock_release(lock);
    }
}


/**
 * @brief Get the process associated with the currently running thread.
 *
 * @return A pointer to the current thread's process
 *
 * @sideeffect None.
 */
struct process * running_thread_process(void) {
    return TP->proc;
}

/**
 * @brief Get the process associated with a specific thread ID.
 *

 * @param tid The thread ID whose process pointer is to be retrieved.
 * @return The pointer to the associated process
 *
 * @sideeffect None.
 */
struct process * thread_process(int tid) {
    if (tid < 0 || tid >= NTHR || thrtab[tid] == NULL)
        return NULL;
    return thrtab[tid]->proc;
}

/**
 * @brief Match a process struct to a thread.
 * 
 * @param tid   The thread ID to bind the process to.
 * @param proc  Pointer to the process to associate, or NULL to unbind.
 *
 * @return None.
 *
 * @sideeffect Modifies the internal state of the thread in thrtab.
 */
void thread_set_process(int tid, struct process * proc) {
    if (tid < 0 || tid >= NTHR || thrtab[tid] == NULL)
        return;
    thrtab[tid]->proc = proc;

}


/**
 * @brief Return the sscratch pointer value for the given thread.
 *
 * @param None
 * @return void* The address to use as sscratch (trap frame save location)
 */
void * thread_sscratch_top(void) {
    return (void*)TP->stack_anchor - sizeof(struct trap_frame);
}