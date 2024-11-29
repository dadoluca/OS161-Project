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
 * @brief exec_bootstrap, used to create an exec throttle semaphore
 * 
 * @return doesn't have return parameter
 */
#if OPT_SHELL
void exec_bootstrap(void) {

    /* SEM BOOTSTRAP */
    execthrottle = sem_create("exec", EXEC_BIGBUF_THROTTLE);
	if (execthrottle == NULL) {
		panic("- Error - while creating an exec throttle semaphore\n");
	}
}
#endif

/**
 * @brief argbuf_init, used to initialize an argv buffer.
 * 
 * @param buf used as buffer, to store the infos
 * 
 * @return doesn't have return parameter
 */
#if OPT_SHELL
void argbuf_init(argbuf_t *buf) {
	buf->data = NULL;
	buf->len = 0;
	buf->max = 0;
	buf->nargs = 0;
	buf->tooksem = false;
}
#endif

/**
 * @brief argbuf_copyin, used to copy an argv from the user side to the kernel side
 * 
 * @param buf used as buffer in the kernel side
 * @param uargv used as argv in the user side
 * 
 * @return an error in case of failure or 0 in case of success
 */
#if OPT_SHELL
int argbuf_copyin(argbuf_t *buf, userptr_t uargv) {
    userptr_t thisarg;
	size_t thisarglen;
	int result;

	while (true) {

		result = copyin(uargv, &thisarg, sizeof(userptr_t));
		if (result) {
			return result;
		}

		/* checking if the array is ended */
		if (thisarg == NULL) {
			break;
		}

		result = copyinstr(thisarg, buf->data + buf->len, buf->max - buf->len, &thisarglen);
		if (result == ENAMETOOLONG) {
			return E2BIG;
		} else if (result) {
			return result;
		}

		buf->len += thisarglen;
		uargv += sizeof(userptr_t);
		buf->nargs++;
	}

	return 0;
}
#endif

/**
 * @brief argbuf_copyout, used to copy an argv from the kernel side to the user side
 * 
 * @param buf used as buffer in the kernel side
 * @param ustackp used as user stack pointer
 * @param argc_ret indicating the number of arguments copied
 * @param uargv_ret used as user argv
 * 
 * @return an error in case of failure or 0 in case of success 
 */
#if OPT_SHELL
int argbuf_copyout(argbuf_t *buf, vaddr_t *ustackp, int *argc_ret, userptr_t *uargv_ret) {
	vaddr_t ustack;
	userptr_t ustringbase, uargvbase, uargv_i;
	userptr_t thisarg;
	size_t thisarglen;
	size_t pos;
	int result;

	ustack = *ustackp;

	/* allocating space */
	ustack -= buf->len;
	ustack -= (ustack & (sizeof(void *) - 1));
	ustringbase = (userptr_t)ustack;

	ustack -= (buf->nargs + 1) * sizeof(userptr_t);
	uargvbase = (userptr_t)ustack;

	/* copying the data */
	pos = 0;
	uargv_i = uargvbase;
	while (pos < buf->len) {
		/* the user address of the string will be ustringbase + pos. */
		thisarg = ustringbase + pos;

		/* putting the user address into the argv array */
		result = copyout(&thisarg, uargv_i, sizeof(thisarg));
		if (result) {
			return result;
		}

		result = copyoutstr(buf->data + pos, thisarg, buf->len - pos, &thisarglen);
		if (result) {
			return result;
		}

        /* done because thisarglen include \0 */
		pos += thisarglen;
		uargv_i += sizeof(thisarg);
	}

	KASSERT(pos == buf->len);

	/* adding the NULL */
	thisarg = NULL;
	result = copyout(&thisarg, uargv_i, sizeof(userptr_t));
	if (result) {
		return result;
	}

	*ustackp = ustack;
	*argc_ret = buf->nargs;
	*uargv_ret = uargvbase;
	return 0;

}
#endif


/**
 * @brief argbuf_allocate, used to initialize the size of the buffer
 * 
 * @param buf used as argv buffer
 * @param size indicate the size of the buffer
 * 
 * @return an error in case of failure or 0 in case of success 
 */
#if OPT_SHELL
int argbuf_allocate(argbuf_t *buf, size_t size) {

    /* allocating space for the data */
    buf->data = (char *) kmalloc(size * sizeof(char));
	if (buf->data == NULL) {
		return ENOMEM;
	}
	buf->max = size;
	return 0;
}
#endif

/**
 * @brief argbuf_cleanup, used to free the memory of the given buffer
 * 
 * @param buf used as buffer
 * 
 * @return doesn't have any return value
 */
#if OPT_SHELL
void argbuf_cleanup(argbuf_t *buf) {
    /* cleaning the data */
    if (buf->data != NULL) {
		kfree(buf->data);
		buf->data = NULL;
	}

	buf->len = 0;
	buf->max = 0;
	buf->nargs = 0;

    /* cleaning the semaphore */
	if (buf->tooksem) {
		V(execthrottle);
		buf->tooksem = false;
	}
}
#endif

/**
 * @brief argbuf_fromuser, used as wrapper function to copy an argv from the user side to the kernel side
 * 
 * @param buf used as buffer in the kernel side
 * @param uargv used as argv in the user side
 * 
 * @return an error in case of failure or 0 in case of success 
 */
#if OPT_SHELL
int argbuf_fromuser(argbuf_t *buf, userptr_t uargv) {
    int result;

	result = argbuf_allocate(buf, PAGE_SIZE);
	if (result) {
		return result;
	}

	/* copying from the user side to the kernel side */
	result = argbuf_copyin(buf, uargv);
	if (result == E2BIG) {
		argbuf_cleanup(buf);
		argbuf_init(buf);
        if (buf == NULL) {
            return ENOMEM;
        }

		/* waiting on the semaphore, to throttle the allocation */
		P(execthrottle);
		buf->tooksem = true;

		result = argbuf_allocate(buf, ARG_MAX);
		if (result) {
			return result;
		}

		result = argbuf_copyin(buf, uargv);
	}

	return result;
}
#endif

/**
 * @brief leadexec, used to load an executable
 * 
 * @param path used as pathname of the executable to run
 * @param entrypoint used as virtual address of the starting point
 * @param stackptr used as virtual address of the stack
 * 
 * @return an error in case of failure or 0 in case of success 
 */
#if OPT_SHELL
int loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr) {

	struct addrspace *newas, *oldas;
	struct vnode *vn;
	char *newname;
	int err;

	/* extracting the new name from the thread */
	newname = kstrdup(path);
	if (newname == NULL) {
		return ENOMEM;
	}

	/* opening the file with path as pathname */
	err = vfs_open(path, O_RDONLY, 0, &vn);
	if (err) {
		kfree(newname);
		return err;
	}

	/* making a new address space */
	newas = as_create();
	if (newas == NULL) {
		vfs_close(vn);
		kfree(newname);
		return ENOMEM;
	}

	/* replacing the old address spaces, and activating the new one */
	oldas = proc_setas(newas);
	as_activate();

 	/* Loading the executable. If it fails, restore the old address space and (re-)activate it */
	err = load_elf(vn, entrypoint);
	if (err) {
		vfs_close(vn);
		proc_setas(oldas);
		as_activate();
		as_destroy(newas);
		kfree(newname);
		return err;
	}

	/* closing the file */
	vfs_close(vn);

	/* defining the user stack in the address space */
	err = as_define_stack(newas, stackptr);
	if (err) {
		proc_setas(oldas);
		as_activate();
		as_destroy(newas);
		kfree(newname);
		return err;
        }

	/* destroying the old address space */
	if (oldas) {
		as_destroy(oldas);
	}

	kfree(curthread->t_name);
	curthread->t_name = newname;

	return 0;
}
#endif