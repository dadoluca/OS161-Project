#ifndef _EXEC_H_
#define _EXEC_H_

#include <kern/unistd.h>
#include <types.h>
#include <kern/errno.h>
#include <vm.h>
#include <limits.h>
#include <current.h>
#include <kern/fcntl.h>
#include "opt-shell.h"

/*
 * Throttle to limit the number of processes in exec at once. Or,
 * rather, the number trying to use large exec buffers at once.
 */
#define EXEC_BIGBUF_THROTTLE 1
#if OPT_SHELL
struct semaphore *execthrottle;
#endif

void exec_bootstrap(void);

#endif