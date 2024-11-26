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

/**
 * @brief Initialize an argv buffer.
 * 
 */
#if OPT_SHELL
void argbuf_init(argbuf_t *buf) {

    /* BUFFER INIT */
	buf->data = NULL;
	buf->len = 0;
	buf->max = 0;
	buf->nargs = 0;
	buf->tooksem = false;

}
#endif

/**
 * @brief Copy an argv from user side to kernel side
 * 
 * @param buf buffer in the kernel side
 * @param uargv argv in the user side
 * @return zero on success, an error value in case of failure 
 */
#if OPT_SHELL
int argbuf_copyin(argbuf_t *buf, userptr_t uargv) {

    userptr_t thisarg;
	size_t thisarglen;
	int result;

	/* LOOP EVERY ARGV, COPYING EACH STRING ONE AT A TIME */
	while (true) {

		/*
		 * First, grab the pointer at argv.
		 * (argv is incremented at the end of the loop)
		 */
		result = copyin(uargv, &thisarg, sizeof(userptr_t));
		if (result) {
			return result;
		}

		/* CHECK ENDS OF THE ARRAY */
		if (thisarg == NULL) {
			break;
		}

		/* USE THE POINTER TO FETCH THE ARGUMENT STRING */
		result = copyinstr(thisarg, buf->data + buf->len, buf->max - buf->len, &thisarglen);
		if (result == ENAMETOOLONG) {
			return E2BIG;
		} else if (result) {
			return result;
		}

		/* MOVE TO THE NEXT ARGUMENT. Note: thisarglen includes the \0. */
		buf->len += thisarglen;
		uargv += sizeof(userptr_t);
		buf->nargs++;
	}

	return 0;
}
#endif

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
int argbuf_copyout(argbuf_t *buf, vaddr_t *ustackp, int *argc_ret, userptr_t *uargv_ret) {

	vaddr_t ustack;
	userptr_t ustringbase, uargvbase, uargv_i;
	userptr_t thisarg;
	size_t thisarglen;
	size_t pos;
	int result;

	/* Begin the stack at the passed in top. */
	ustack = *ustackp;

	/*
	 * Allocate space.
	 *
	 * buf->pos is the amount of space used by the strings; put that
	 * first, then align the stack, then make space for the argv
	 * pointers. Allow an extra slot for the ending NULL.
	 */

	ustack -= buf->len;
	ustack -= (ustack & (sizeof(void *) - 1));
	ustringbase = (userptr_t)ustack;

	ustack -= (buf->nargs + 1) * sizeof(userptr_t);
	uargvbase = (userptr_t)ustack;

	/* Now copy the data out. */
	pos = 0;
	uargv_i = uargvbase;
	while (pos < buf->len) {
		/* The user address of the string will be ustringbase + pos. */
		thisarg = ustringbase + pos;

		/* Place it in the argv array. */
		result = copyout(&thisarg, uargv_i, sizeof(thisarg));
		if (result) {
			return result;
		}

		/* Push out the string. */
		result = copyoutstr(buf->data + pos, thisarg, buf->len - pos, &thisarglen);
		if (result) {
			return result;
		}

		/* thisarglen includes the \0 */
		pos += thisarglen;
		uargv_i += sizeof(thisarg);
	}
	/* Should have come out even... */
	KASSERT(pos == buf->len);

	/* Add the NULL. */
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
 * @brief Initialize size of the buffer
 * 
 * @param buf argv buffer
 * @param size size of the buffer
 * @return zero on success, an error value in case of failure 
 */
#if OPT_SHELL
int argbuf_allocate(argbuf_t *buf, size_t size) {

    /* ALLOCATING SPACE FOR DATA */
    buf->data = (char *) kmalloc(size * sizeof(char));
	if (buf->data == NULL) {
		return ENOMEM;
	}
	buf->max = size;
	return 0;
}
#endif

/**
 * @brief Free memory related to the given buffer
 * 
 * @param buf buffer
 */
#if OPT_SHELL
void argbuf_cleanup(argbuf_t *buf) {

    /* DATA CLEANUP */
    if (buf->data != NULL) {
		kfree(buf->data);
		buf->data = NULL;
	}

	buf->len = 0;
	buf->max = 0;
	buf->nargs = 0;

    /* SEM CLEANUP */
	if (buf->tooksem) {
		V(execthrottle);
		buf->tooksem = false;
	}
}
#endif

/**
 * @brief Wrapper function to copy an argv from user side to kernel side
 * 
 * @param buf buffer in the kernel side
 * @param uargv argv in the user side
 * @return zero on success, an error value in case of failure 
 */
#if OPT_SHELL
int argbuf_fromuser(argbuf_t *buf, userptr_t uargv) {

    int result;

	/* ATTEMPT WITH A SMALL BUFFER */
	result = argbuf_allocate(buf, PAGE_SIZE);
	if (result) {
		return result;
	}

	/* ACTUAL COPY FROM USER SIDE TO KERNEL SIDE */
	result = argbuf_copyin(buf, uargv);
	if (result == E2BIG) {
		/*
		 * Try again with the full-size buffer. Just start
		 * over instead of trying to keep the page we already
		 * did; this is a bit inefficient but it's not that
		 * important.
		 */
		argbuf_cleanup(buf);
		argbuf_init(buf);
        if (buf == NULL) {
            return ENOMEM;
        }

		/* Wait on the semaphore, to throttle this allocation */
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
 * @brief Load an executable (common code with runprogram)
 * 
 * @param path pathname of the executable to run
 * @param entrypoint virtual address of the starting point
 * @param stackptr virtual address of the stack
 * @return zero on success, an error value in case of failure
 */
#if OPT_SHELL
int loadexec(char *path, vaddr_t *entrypoint, vaddr_t *stackptr) {

	struct addrspace *newas, *oldas;
	struct vnode *vn;
	char *newname;
	int err;

	/* NEW NAME FROM THREAD */
	newname = kstrdup(path);
	if (newname == NULL) {
		return ENOMEM;
	}

	/* open the file. */
	err = vfs_open(path, O_RDONLY, 0, &vn);
	if (err) {
		kfree(newname);
		return err;
	}

	/* make a new address space. */
	newas = as_create();
	if (newas == NULL) {
		vfs_close(vn);
		kfree(newname);
		return ENOMEM;
	}

	/* replace address spaces, and activate the new one */
	oldas = proc_setas(newas);
	as_activate();

 	/*
	 * Load the executable. If it fails, restore the old address
	 * space and (re-)activate it.
	 */
	err = load_elf(vn, entrypoint);
	if (err) {
		vfs_close(vn);
		proc_setas(oldas);
		as_activate();
		as_destroy(newas);
		kfree(newname);
		return err;
	}

	/* Done with the file now. */
	vfs_close(vn);

	/* Define the user stack in the address space */
	err = as_define_stack(newas, stackptr);
	if (err) {
		proc_setas(oldas);
		as_activate();
		as_destroy(newas);
		kfree(newname);
		return err;
        }

	/*
	 * Wipe out old address space.
	 *
	 * Note: once this is done, execv() must not fail, because there's
	 * nothing left for it to return an error to.
	 */
	if (oldas) {
		as_destroy(oldas);
	}

	/*
	 * Now that we know we're succeeding, change the current thread's
	 * name to reflect the new process.
	 */
	kfree(curthread->t_name);
	curthread->t_name = newname;

	return 0;
}
#endif