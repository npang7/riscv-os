/*! @file syscall.c
    @brief system call handlers 
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/



#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"
#include "process.h"
#include "ioimpl.h"
#include "string.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame * tfr); // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame * tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char ** argv);
static int sysfork(const struct trap_frame * tfr);
static int syswait(int tid);
static int sysprint(const char * msg);
static int sysusleep(unsigned long us);

static int sysdevopen(int fd, const char * name, int instno);
static int sysfsopen(int fd, const char * name);

static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, void * arg);
static int syspipe(int * wfdptr, int * rfdptr);

static int sysfscreate(const char * name); 
static int sysfsdelete(const char * name);

static int sysiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS
//
#define MAX_ARGC 16              // 最多 16 个参数
#define MAX_ARG_TOTAL_LEN 512   // 所有参数字符串最多 512 字节


int copyinto(void *dst, const void *src_user, size_t len) {
    const char *src = (const char *)src_user;
    char *dst_ = (char *)dst;

    for (size_t i = 0; i < len; i++) {
        dst_[i] = src[i];
    }

    return 0;
}

int copyinstr(char *dst, const char *src_user, size_t maxlen) {
    for (size_t i = 0; i < maxlen; i++) {
        char c = src_user[i];
        dst[i] = c;
        if (c == '\0') {
            return 0;  // 成功
        }
    }

    return -1;  // 超长未终止
}




/* void handle_syscall(struct trap_frame * tfr)
 * Inputs: struct trap_frame * tfr - pointer to trap frame struct
 * Outputs: None
 * Description: This function initiates syscall present in trap frame struct and stores the return address into the sepc.
 * Side Effects: It changes the a0 register in the trap frame to store the result.
 */
void handle_syscall(struct trap_frame * tfr) {
    // store the return address into the sepc
    tfr->sepc += 4;
    // handle the syscall
    int64_t ret = syscall(tfr);
    // return by storing the return value in a0
    tfr->a0 = ret;
}

// INTERNAL FUNCTION DEFINITIONS
//



/* int64_t syscall(const struct trap_frame * tfr)
 * Inputs: const struct trap_frame * tfr - pointer to trap frame struct
 * Outputs: return result of syscall or -ENOTSUP if the syscall is not supported
 * Description: This function calls specified syscall and passes arguments.
 * Side Effects: None
 */
int64_t syscall(const struct trap_frame * tfr) {
    // get the system call number from a7
    int syscall_num = tfr->a7;

    // jump to a system call based on the specified system call number
    switch (syscall_num) {
        case SYSCALL_EXIT:
            return sysexit();
        case SYSCALL_EXEC:
            return sysexec(tfr->a0, tfr->a1, (char **) tfr->a2);
        case SYSCALL_WAIT:
            return syswait(tfr->a0);
        case SYSCALL_PRINT:
            return sysprint((const char *) tfr->a0);
        case SYSCALL_USLEEP:
            return sysusleep(tfr->a0);
        case SYSCALL_DEVOPEN:
            return sysdevopen(tfr->a0, (const char *) tfr->a1, tfr->a2);
        case SYSCALL_FSOPEN:
            return sysfsopen(tfr->a0, (const char *) tfr->a1);
        case SYSCALL_CLOSE:
            return sysclose(tfr->a0);
        case SYSCALL_READ:
            return sysread(tfr->a0, (void *) tfr->a1, tfr->a2);
        case SYSCALL_WRITE:
            return syswrite(tfr->a0, (const void *) tfr->a1, tfr->a2);
        case SYSCALL_IOCTL:
            return sysioctl(tfr->a0, tfr->a1, (void *) tfr->a2);
        case SYSCALL_FSCREATE:
            return sysfscreate((const char *) tfr->a0);
        case SYSCALL_FSDELETE:
            return sysfsdelete((const char *) tfr->a0);
        case SYSCALL_FORK:
            return sysfork(tfr);
        case SYSCALL_PIPE:
            return syspipe((int *) tfr->a0, (int *) tfr->a1);
        case SYSCALL_IODUP:
            return sysiodup(tfr->a0, tfr->a1);

        // if the syscall is not supported
        default:
            return -ENOTSUP;
    }
}

/* int sysexit(void)
 * Inputs: None
 * Outputs: return 0 if succeed.
 * Description: This function exits the currently running process.
 * Side Effects: None
 */
int sysexit(void) {
    // call the kernel function to exit the current process
    process_exit();
    return 0;
}

/* int sysprint(const char * msg)
 * Inputs: const char * msg - string msg in userspace
 * Outputs: return 0 on sucess else error codes
 * Description: This function prints msg to the console.
 * Side Effects: None
 */
int sysprint(const char * msg) {
    // check whether the input msg is valid
    if (validate_vstr(msg, PTE_U | PTE_R) < 0) {
        return -EINVAL;
    }

    // get the current thread name and print messages to the console
    kprintf("<%s:%d> %s\n", running_thread_name(), running_thread(), msg);

    // return 0 on success
    return 0;
}

/* int sysexec(int fd, int argc, char ** argv)
 * Inputs: int fd - file descriptor idx
 *         int argc - number of arguments in argv
 *         char ** argv - array of arguments for multiple args
 * Outputs: return result of executing the program, else -EBADFD on invalid file descriptors
 * Description: This function executes new process given a executable and arguments. It will
 *              execute the program associated with the io device associated with fd.
 * Side Effects: None
 */
/*int sysexec(int fd, int argc, char ** argv) {
    // check whether the file descriptor is in a valid range
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }

    // check if there is file/device associated with the provided fd
    if (current_process()->iotab[fd] == NULL) {
        return -EBADFD;
    }

    int result;
    // call process_exec to execute the new program
    result = process_exec(current_process()->iotab[fd], argc, argv);

    // if execution fails, return an error code
    if (result < 0) {
        // do relevant cleanup
        ioclose(current_process()->iotab[fd]);
        
        // clear the io object in the descriptor table
        current_process()->iotab[fd] = NULL;

        return result;
    }

    // avoid warnings
    return 0;
}*/
int sysexec(int fd, int argc, char **argv_user) {
    if (fd < 0 || fd >= PROCESS_IOMAX)
        return -EBADFD;
    if (current_process()->iotab[fd] == NULL)
        return -EBADFD;
    if (argc < 0 || argc > MAX_ARGC)
        return -EINVAL;

    int i, result;
    char *argv_kernel[MAX_ARGC + 1];   // 最多 argc + NULL
    char argbuf[MAX_ARG_TOTAL_LEN];   // 用于单个字符串临时拷贝
    int offset = 0;

    // 安全地从用户空间拷贝 argv[]
    for (i = 0; i < argc; i++) {
        uintptr_t user_ptr;

        // 先拷贝指针
        if (copyinto(&user_ptr, &argv_user[i], sizeof(user_ptr)) < 0)
            return -1;

        // 拷贝字符串本体
        if (copyinstr(argbuf + offset, (const char *)user_ptr, MAX_ARG_TOTAL_LEN - offset) < 0)
            return -1;

        // 保存 argv[i] 指针指向的内核地址
        argv_kernel[i] = argbuf + offset;
        offset += strlen(argv_kernel[i]) + 1;
    }

    argv_kernel[argc] = NULL;

    // 调用内核内部 exec
    result = process_exec(current_process()->iotab[fd], argc, argv_kernel);

    if (result < 0) {
        ioclose(current_process()->iotab[fd]);
        current_process()->iotab[fd] = NULL;
        return result;
    }

    return 0;
}


/* int syswait(int tid)
 * Inputs: int tid - thread_id
 * Outputs: return result of waiting or -EINVAL on error/bad input
 * Description: This function sleeps process until a specified child process completes.
 * Side Effects: None
 */
int syswait(int tid) {
    // wait for a specific child process completes
    return thread_join(tid);
}

/* int sysusleep(unsigned long us)
 * Inputs: unsigned long us - time in microseconds for process to sleep
 * Outputs: return 0
 * Description: This function sleeps process until the specified amount of time has passed.
 * Side Effects: None
 */
int sysusleep(unsigned long us) {
    // return directly
    if (us == 0) {
        return 0;
    }

    // set the sleep time
    sleep_us(us);

    return 0;
}

/* int sysdevopen(int fd, const char * name, int instno)
 * Inputs: int fd - file descriptor number
 *         const char * name - string name of device
 *         int instno - instance number of virtualized device
 * Outputs: return fd number if successful, else return error that occured.
 * Description: This function opens unique device instance for current process. It will allocate a valid and unused
 *              file descriptor if fd = -1. Otherwise, it will use the file descriptor given, if it is valid and unused.
 * Side Effects: It adds new io object to the file descriptor table.
 */
int sysdevopen(int fd, const char * name, int instno) {
    struct io * ioptr;
    int result;

    // check whether the given device name is valid
    if (name == NULL) {
        return -EINVAL;
    }

    // if fd < 0, requests next available descriptor number
    if (fd < 0) {
        // find the next available file descriptor
        for (fd = 0; fd < PROCESS_IOMAX; fd++) {
            if (current_process()->iotab[fd] == NULL) {
                break;
            }
        }
        if (fd == PROCESS_IOMAX) {
            return -EMFILE; // there are no available fds
        }
    }
    // if fd >= 0, requests specific file descriptor number
    else {
        // ensure the requested file descriptor is within a valid range
        if (fd >= PROCESS_IOMAX || current_process()->iotab[fd] != NULL) {
            return -EBADFD; // the fd is not valid or unused
        }
    }

    // open the device
    result = open_device(name, instno, &ioptr);
    // return an error code if opening the device fails
    if (result < 0) {
        return result;
    }

    // add the io object to the file descriptor table
    current_process()->iotab[fd] = ioptr;

    // return the file descriptor if succeed
    return fd;
}

/* int sysfsopen(int fd, const char * name)
 * Inputs: int fd - file descriptor number
 *         const char * name - string name of file
 * Outputs: return fd number if successful, else return error that occured.
 * Description: This function opens file in filesystem for current process. It will allocate a valid and unused
 *              file descriptor if fd = -1. Otherwise, it will use the file descriptor given, if it is valid and unused.
 * Side Effects: It adds new io object to the file descriptor table.
 */
int sysfsopen(int fd, const char * name) {
    struct io *ioptr;
    int result;

    // check whether the given file name is valid
    if (name == NULL) {
        return -EINVAL;
    }

    // if fd < 0, requests next available descriptor number
    if (fd < 0) {
        // find the next available file descriptor
        for (fd = 0; fd < PROCESS_IOMAX; fd++) {
            if (current_process()->iotab[fd] == NULL) {
                break;
            }
        }
        if (fd == PROCESS_IOMAX) {
            return -EMFILE; // there are no available fds
        }
    }
    // if fd >= 0, requests specific file descriptor number
    else {
        // ensure the requested file descriptor is within a valid range
        if (fd >= PROCESS_IOMAX || current_process()->iotab[fd] != NULL) {
            return -EBADFD; // the fd is not valid or unused
        }
    }

    // open the file
    result = fsopen(name, &ioptr);
    // return an error code if opening the file fails
    if (result < 0) {
        return result;
    }

    // add the io object to the file descriptor table
    current_process()->iotab[fd] = ioptr;

    // return the file descriptor if succeed
    return fd;
}

/* int sysclose(int fd)
 * Inputs: int fd - file descriptor number
 * Outputs: return 0 on success, -EBADFD on invalid file descriptor or if no file/device is associated with the provided fd
 * Description: This function closes the file or device associated with the provided fd.
 * Side Effects: It deletes io object in the file descriptor table.
 */
int sysclose(int fd) {
    // check whether the file descriptor is in a valid range
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }

    // check if there is file/device associated with the provided fd
    if (current_process()->iotab[fd] == NULL) {
        return -EBADFD;
    }

    struct io *ioptr = current_process()->iotab[fd];

    // call the close function of the device interface
    ioclose(ioptr);
    //ioptr->intf->close(ioptr);

    // clear the io object in the descriptor table
    current_process()->iotab[fd] = NULL;

    return 0;
}

/* long sysread(int fd, void * buf, size_t bufsz)
 * Inputs: int fd - file descriptor number
 *         void * buf - pointer to buffer
 *         size_t bufsz - number of bytes to be read
 * Outputs: return number of bytes actually read
 * Description: This function reads from the file/device associated with fd into the given buffer (buf).
 *              It make sures that at most bufsz number of bytes is read into buf, among other validity checks.
 * Side Effects: The content in buf is changed.
 */
long sysread(int fd, void * buf, size_t bufsz) {
    // check whether the file descriptor is in a valid range
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }

    // check whether the buffer is valid
    if (buf == NULL) {
        return -EINVAL;
    }
    if (validate_vptr(buf, bufsz, PTE_U | PTE_W) < 0) {
        return -EINVAL;
    }

    // check if there is file/device associated with the provided fd
    if (current_process()->iotab[fd] == NULL) {
        return -EBADFD;
    }

    struct io * ioptr = current_process()->iotab[fd];
    // call the read function of the file interface
    long result = ioptr->intf->read(ioptr, buf, bufsz);

    //assert(result <= bufsz);

    // return number of bytes read on success
    return result;
}

/* long syswrite(int fd, const void * buf, size_t len)
 * Inputs: int fd - file descriptor number
 *         void * buf - pointer to buffer
 *         size_t len - number of bytes to be written
 * Outputs: return number of bytes actually written
 * Description: This function writes to the file/device from the provided buffer (buf).
 *              It make sures that at most len number of bytes is written from the buf into the file/device, among other validity checks.
 * Side Effects: The content of the opened file/device is changed.
 */
long syswrite(int fd, const void * buf, size_t len) {
    // check whether the file descriptor is in a valid range
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }

    // check whether the buffer is valid
    if (buf == NULL) {
        return -EINVAL;
    }

    // check if there is file/device associated with the provided fd
    if (current_process()->iotab[fd] == NULL) {
        return -EBADFD;
    }

    struct io *ioptr = current_process()->iotab[fd];
    // call the write function of the file interface
    long result = ioptr->intf->write(ioptr, buf, len);

    //assert(result <= len);
    
    // return number of bytes written on success
    return result;
}

/* int sysioctl(int fd, int cmd, void * arg)
 * Inputs: int fd - file descriptor number
 *         int cmd - which IOCTL to call
 *         void * arg - pointer to argument
 * Outputs: return the result of the IOCTL call on the device or an error code
 * Description: This function calls device ioctl commands for a given device instance, specified by fd.
 * Side Effects: None
 */
int sysioctl(int fd, int cmd, void * arg) {
    // check whether the file descriptor is in a valid range
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }

    // check if there is file/device associated with the provided fd
    if (current_process()->iotab[fd] == NULL) {
        return -EBADFD;
    }

    struct io *ioptr = current_process()->iotab[fd];
    // call the ioctl function of the device or file interface
    return ioptr->intf->cntl(ioptr, cmd, arg);
}

/* int sysfscreate(const char* name)
 * Inputs: const char * name - a string that is the name of the new file
 * Outputs: return 0 on success, negative value on error
 * Description: This function creates a new file named name of length 0 in the filesystem.
 * Side Effects: None
 */
int sysfscreate(const char * name) {
    // check whether the name is valid
    if (name == NULL) {
        return -EINVAL;
    }

    return fscreate(name);
}

/* int sysfsdelete(const char* name)
 * Inputs: const char * name - a string that is the name of the file to delete
 * Outputs: return 0 on success, negative value on error
 * Description: This function deletes a file named name from the filesystem.
 * Side Effects: None
 */
int sysfsdelete(const char * name) {
    // check whether the name is valid
    if (name == NULL) {
        return -EINVAL;
    }

    return fsdelete(name);
}

/* int sysfork(const struct trap_frame * tfr)
 * Inputs: const struct trap_frame * tfr - pointer to the trap frame
 * Outputs: return result of forking the process
 * Description: This function will create a new child process that is a clone of the caller.
 * Side Effects: None
 */
/*
 * Function Name: sysfork
 * Description: This function creates a new child process by allocating a process slot, copying the 
 *              parent's I/O table, and forking a new thread for the child. The child process inherits 
 *              resources from the parent. The parent process receives the child's process ID (PID), 
 *              while the child process returns 0 to indicate success.
 * Inputs: tfr - a pointer to the trap frame of the parent process, used to initialize the child process
 * Outputs: Returns the PID of the child process if successful, or a negative error code on failure
 * Side Effect: None
 */
int sysfork(const struct trap_frame * tfr) {
    // invalid argument, return directly
    if (tfr == NULL) {
        return -EINVAL;
    }

    return process_fork(tfr);
}

/* int sysiodup(int oldfd, int newfd)
 * Inputs: int oldfd - old file descriptor number
 *         int newfd - new file descriptor number
 * Outputs: return fd number if sucessful; else return error on invalid file descriptor or empty file descriptor
 * Description: This function duplicates a file description. It allocates a new file descriptor
 *              that refers to the same open file description as the descriptor oldfd.
 * Side Effects: May close the target file descriptor if already open.
 */
int sysiodup(int oldfd, int newfd) {
    // check whether the file descriptors are in a valid range
    if (oldfd < 0 || oldfd >= PROCESS_IOMAX || newfd < 0 || newfd >= PROCESS_IOMAX) {
        return -EBADFD;
    }


    // check whether the old file descriptor is empty
    if (current_process()->iotab[oldfd] == NULL) {
        return -EBADFD;
    }

    // return directly if they are the same
    if (oldfd == newfd) {
        return newfd;
    }

    // if newfd is already open, close it first
    if (current_process()->iotab[newfd] != NULL) {
        sysclose(newfd);//refcnt减几次？？是否只在归零时释放资源 seekio似乎不是？查看其他非seek的设备open close free refcnt是否正确? 能不能自己改io.c
    }

    // duplicate the file description
    current_process()->iotab[newfd] = current_process()->iotab[oldfd];

    // increment the refcnt ??加几次？？？
    int ref = ++current_process()->iotab[newfd]->refcnt;
    kprintf("refcnt: %d\n", ref);


    return newfd;
}


int syspipe(int *wfdptr_user, int *rfdptr_user) {
    kprintf("[DEBUG syspipe] user ptrs: wfd=%p, rfd=%p\n", wfdptr_user, rfdptr_user);
    kprintf("enter1\n");
    if (!wfdptr_user || !rfdptr_user) {
        kprintf("syspipe got NULL argument: wfdptr=%p, rfdptr=%p\n", wfdptr_user, rfdptr_user);
        return -100;
    }
    kprintf("enter2\n");
    int wfd = -1, rfd = -1;

    // Safely copy values from user space
    if (copyin(&wfd, wfdptr_user, sizeof(int)) < 0 ||
        copyin(&rfd, rfdptr_user, sizeof(int)) < 0) {
        kprintf("syspipe copyin failed\n");
        return -9;
    }
    kprintf("enter3\n");

    struct io **iotab = current_process()->iotab;
    
    kprintf("enter4\n");

    // Allocate new FDs if needed
    if (wfd < 0) {
        for (wfd = 0; wfd < PROCESS_IOMAX; wfd++) {
            if (iotab[wfd] == NULL) break;
        }
        if (wfd == PROCESS_IOMAX)
            return -200;
    }
    kprintf("enter5\n");
    if (rfd < 0) {
        for (rfd = 0; rfd < PROCESS_IOMAX; rfd++) {
            if (rfd != wfd && iotab[rfd] == NULL) {
                break;
            }
        }
        if (rfd == PROCESS_IOMAX)
            return -300;
    }
    

    // if (rfd < 0) {
    //     for (rfd = 0; rfd < PROCESS_IOMAX; rfd++) {
    //         kprintf("111\n");
    //         if (iotab[rfd] == NULL) {
    //             kprintf("222\n");
    //             break;}
    //     }
    //     if (rfd == PROCESS_IOMAX || rfd == wfd)
    //         kprintf("333\n");
    //         return -300;
    // }
    kprintf("enter6\n");

    // Validate fd range and check for reuse
    if ((wfd >= 0 && wfd < PROCESS_IOMAX && iotab[wfd] != NULL) ||
        (rfd >= 0 && rfd < PROCESS_IOMAX && iotab[rfd] != NULL) ||
        (wfd == rfd)) {
        return -400;
    }
    kprintf("enter7\n");

    if (wfd < 0 || wfd >= PROCESS_IOMAX || rfd < 0 || rfd >= PROCESS_IOMAX)
        return -EBADFD;

    // Create pipe endpoints
    struct io *wio, *rio;
    create_pipe(&wio, &rio);

    iotab[wfd] = wio;
    iotab[rfd] = rio;

    // Copy back fds to user
    if (copyout(wfdptr_user, &wfd, sizeof(int)) < 0 ||
        copyout(rfdptr_user, &rfd, sizeof(int)) < 0) {
        // Clean up if failure
        ioclose(wio);
        ioclose(rio);
        return -10;
    }

    return 0;
}