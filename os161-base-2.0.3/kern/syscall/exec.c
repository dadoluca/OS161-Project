#include <types.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <vfs.h>
#include <uio.h>
#include <synch.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/unistd.h>
#include <endian.h>
#include <stat.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <kern/wait.h>
#include <mips/trapframe.h>
#include <syscall.h>
#include "exec.h"

/**
 * @brief Set things up.
 */
#if OPT_SHELL
void exec_bootstrap(void) {

    /* SEM BOOTSTRAP */
    execthrottle = sem_create("exec", EXEC_BIGBUF_THROTTLE);
 if (execthrottle == NULL) {
  panic("Cannot create exec throttle semaphore\n");
 }
}
#endif