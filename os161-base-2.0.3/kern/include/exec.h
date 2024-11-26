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
#define EXEC_BIGBUF_THROTTLE	1
#if OPT_SHELL
struct semaphore *execthrottle;
#endif

/**
 * @brief ARGV BUFFER
 *      
 *  An abstraction that wraps an argv in the kernel side during the execv()
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

/**
 * @brief Set things up.
 */
#if OPT_SHELL
void exec_bootstrap(void);
#endif

/**
 * @brief Initialize an argv buffer.
 * 
 */
#if OPT_SHELL
void argbuf_init(argbuf_t *buf);
#endif

/**
 * @brief Wrapper function to copy an argv from user side to kernel side
 * 
 * @param buf buffer in the kernel side
 * @param uargv argv in the user side
 * @return zero on success, an error value in case of failure 
 */
#if OPT_SHELL
int argbuf_fromuser(argbuf_t *buf, userptr_t uargv);
#endif

/**
 * @brief Copy an argv from user side to kernel side
 * 
 * @param buf buffer in the kernel side
 * @param uargv argv in the user side
 * @return zero on success, an error value in case of failure 
 */
int argbuf_copyin(argbuf_t *buf, userptr_t uargv);

/**
 * @brief Copy an argv from kernel side to user side
 * 
 * @param buf buffer in the kernel side
 * @param ustackp user stack pointer
 * @param argc_ret number of arguments copied
 * @param uargv_ret user argv
 * @return zero on success, an error value in case of failure 
 */
#if OPT_SHELL
int argbuf_copyout(argbuf_t *buf, vaddr_t *ustackp, int *argc_ret, userptr_t *uargv_ret);
#endif

/**
 * @brief Initialize size of the buffer
 * 
 * @param buf argv buffer
 * @param size size of the buffer
 * @return zero on success, an error value in case of failure 
 */
#if OPT_SHELL
int argbuf_allocate(argbuf_t *buf, size_t size);
#endif

/**
 * @brief Free memory related to the given buffer
 * 
 * @param buf buffer
 */
#if OPT_SHELL
void argbuf_cleanup(argbuf_t *buf);
#endif

/**
 * @brief Load an executable (common code with runprogram)
 * 
 * @param path pathname of the executable to run
 * @param entrypoint virtual address of the starting point
 * @param stackptr virtual address of the stack
 * @return zero on success, an error value in case of failure
 */
#if OPT_SHELL
int loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr);
#endif

#endif /* _EXEC_H_ */
