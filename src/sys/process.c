// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"
#include "ioimpl.h"

// COMPILE-TIME PARAMETERS
//


#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//


static int build_stack(void * stack, int argc, char ** argv);

//cp3
static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//


static struct process main_proc;

//cp3
static struct process * proctab[NPROC] = {
    &main_proc
};

// EXPORTED GLOBAL VARIABLES
//
struct condition forked;

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//
/**
 * @brief Initializes the process manager and sets up the main process.
 *
 * @arg None
 * @return None
 * @sideeffects: None
 */
void procmgr_init(void) {
    assert (memory_initialized && heap_initialized);
    assert (!procmgr_initialized);

    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    proctab[0] = &main_proc;  //cp3 mutiple process
    procmgr_initialized = 1;
}

/**
 * @brief Replaces the current thread's memory space with a new user process.
 *
 * This function clears the current virtual memory, loads an ELF executable from "exeio",
 * allocates a user stack and build it, and jumps to the user-mode entry point.
 * 
 * @param exeio The I/O object the ELF file to be executed.
 * @param argc  Argument count passed to the user program
 * @param argv  Argument values passed to the user program
 *
 * @return No return if successful.
 *
 * @sideeffects: Allocates physical pages for user stack.
 */
int process_exec(struct io * exeio, int argc, char ** argv) {
    struct trap_frame tf;
    void (*entry)(void);  // entry point
    // clear old virtual memory space
    reset_active_mspace();
    // load ELF 
    if (elf_load(exeio, &entry) < 0)
        thread_exit();  // failed, thread exit
    
    ioclose(exeio);

    uintptr_t stack_top = UMEM_END_VMA;
    uintptr_t stack_bottom = stack_top - PAGE_SIZE;

    if (!alloc_and_map_range(stack_bottom, PAGE_SIZE, PTE_R | PTE_W | PTE_U))
        thread_exit();  

    // build user stack
    int used = build_stack((void*)stack_bottom, argc, argv);
    if (used < 0)
        thread_exit(); 

    // set up the initial trap frame
    memset(&tf, 0, sizeof(tf));

    tf.sp = (void*)(stack_top - ROUND_UP(used, 16));  
    tf.a0 = argc;                                     
    tf.a1 = (uintptr_t)tf.sp;                         // points to &argv[0]
    tf.sepc = entry;                  

    tf.sstatus = csrr_sstatus();                      
    tf.sstatus |= RISCV_SSTATUS_SPIE;
    tf.sstatus &= ~RISCV_SSTATUS_SPP;

    void *sscratch = thread_sscratch_top();           // set sscratch
    trap_frame_jump(&tf, sscratch);         // jump to user mode
}

/* int process_fork(const struct trap_frame * tfr)
 * Inputs: const struct trap_frame * tfr - pointer to trap frame struct
 * Outputs: return 0 on success, error code on failure
 * Description: This function forks a child process. It creates a new process struct
 *              for the child, copies the parent's I/O objects and spawns a new thread
 *              for the child. The child thread uses the parent's trap frame to return to
 *              U mode, signaling the parent that it is done with the trap frame.
 * Side Effects: It allocates new memory.
 */
int process_fork(const struct trap_frame * tfr) {
    int i;
    // find an available slot for the child process
    struct process * child_proc = NULL;
    for (i = 0; i < NPROC; i++) {
        // if this is an empty slot
        if (proctab[i] == NULL) {
            // allocate memory space for the child process
            child_proc = (struct process *)kmalloc(sizeof(struct process));
            // memory allocation failure
            if (child_proc == NULL) {
                return -EBUSY;
            }
            // initialize the child process space
            memset(child_proc, 0, sizeof(struct process));
            // add the child process to the process list and link them by id
            proctab[i] = child_proc;
            child_proc->idx = i;
            break;
        }
    }
    // no available process slots
    if (child_proc == NULL) {
        return -ENOMEM;
    }

    // copy the parent's I/O table to the child and increment reference counts
    struct process *parent_proc = current_process();
    for (i = 0; i < PROCESS_IOMAX; i++) {
        if (parent_proc->iotab[i] != NULL) {
            child_proc->iotab[i] = parent_proc->iotab[i];
            // increment reference count for shared I/O objects
            child_proc->iotab[i]->refcnt++;
        } else {
            child_proc->iotab[i] = NULL;
        }
    }

    // clone the parent's memory space for the child
    mtag_t child_mtag = clone_active_mspace();
    // memory cloning failure
    if (child_mtag == 0) {
        proctab[child_proc->idx] = NULL;
        kfree(child_proc);
        return -EBUSY;
    }
    child_proc->mtag = child_mtag;

    // copy the trap frame for the child process
    struct trap_frame *child_tf = kmalloc(sizeof(struct trap_frame));
    if (child_tf == NULL) {
        return -ENOMEM;
    }
    memcpy(child_tf, tfr, sizeof(struct trap_frame));

    // the child process returns 0 in fork
    child_tf->a0 = 0;

    // initialize the condition
    condition_init(&forked, "forked");

    // fork a new thread for the child process
    int child_tid = thread_spawn("child", (void(*)(void))fork_func, &forked, child_tf);
    if (child_tid < 0) {
        proctab[child_proc->idx] = NULL;
        kfree(child_tf);
        kfree(child_proc);
        return child_tid;
    }
    // set the corresponding process
    thread_set_process(child_tid, child_proc);
    // save the tid to the process
    child_proc->tid = child_tid;

    // wait for child thread initialization to complete
    condition_wait(&forked);

    // the parent process returns child's pid in fork
    return child_proc->idx;
}

/**
 * @brief Terminates the current process and cleans up its resources.
 *
 * @arg None
 * @return None
 *
 * @sideeffects:Frees the process's memory space.
 */
void process_exit(void) {
    struct process *proc = running_thread_process();
    if (!proc)
        thread_exit();  

    // Close all I/O objects
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        if (proc->iotab[i]) {
            ioclose(proc->iotab[i]);
            proc->iotab[i] = NULL;
        }
    }
    // split current thread from the process
    int tid = running_thread();
    thread_set_process(tid, NULL);
    proctab[proc->idx] = NULL;

    discard_active_mspace();  // Clean up the memory space //cp3
    thread_exit();
}


// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void * stack, int argc, char ** argv) {
    size_t stksz, argsz;
    uintptr_t * newargv;
    char * p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc)
        return -ENOMEM;
    
    stksz = (argc+1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i])+1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert (stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz; //
    p = (char*)(newargv+argc+1);    
                                    //real char content

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack); 
                                                                           
        argsz = strlen(argv[i])+1;  //+1 null char
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0; //set terminate
    return stksz;
}

/* void fork_func(struct condition * done, struct trap_frame * tfr)
 * Inputs: struct condition * done - pointer to condition variable to signal parent
 *         struct trap_frame * tfr - pointer to trap frame struct
 * Outputs: None
 * Description: This is the function to be executed by the child process after fork.
 *              It signals the parent process that it is done with the trap frame,
 *              then jumps to user space.
 * Side Effects: None
 */
void fork_func(struct condition * done, struct trap_frame * tfr) {
    // switch to child memory space
    switch_mspace(current_process()->mtag);
    sfence_vma();

    // signal the parent process that it is done with the trap frame
    condition_broadcast(done);

    // set the sscratch for the child process
    void *sscratch = thread_sscratch_top();
    // jump to user space
    trap_frame_jump(tfr, sscratch);
}
