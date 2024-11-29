
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

/* throttle used to limit the number of processes in exec at once */
#define EXEC_BIGBUF_THROTTLE	1

#if OPT_SHELL
struct semaphore *execthrottle;
#endif

/**
 * @brief argbuf_s, used as argv buffer
 */
#if OPT_SHELL
typedef struct argbuf_s {

	char *data;	
	size_t len;
	size_t max;
	int nargs;
	bool tooksem;

} argbuf_t;
#endif

#if OPT_SHELL
void exec_bootstrap(void);
void argbuf_init(argbuf_t *buf);
int argbuf_fromuser(argbuf_t *buf, userptr_t uargv);
int argbuf_copyin(argbuf_t *buf, userptr_t uargv);
int argbuf_copyout(argbuf_t *buf, vaddr_t *ustackp, int *argc_ret, userptr_t *uargv_ret);
int argbuf_allocate(argbuf_t *buf, size_t size);
void argbuf_cleanup(argbuf_t *buf);
int loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr);
#endif

#endif /* _EXEC_H_ */