#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <current.h>
#include <synch.h>
#include <kern/wait.h>
#include "exec.h"


/**
 * @brief sys__exit, used to exit the process
 * 
 * @param status used to return back the exit status to other function
 * @return doesn't have return parameter
 */
#if OPT_SHELL
void sys__exit(int status) {
    struct proc *proc = curproc;
    proc->p_status = _MKWAIT_EXIT(status);    /* exitcode & 0xff */

    /* removing thread from current process */
    proc_remthread(curthread);

    lock_acquire(proc->p_locklock);
    cv_signal(proc->p_cv, proc->p_locklock);
    lock_release(proc->p_locklock);

    thread_exit();
    
    panic("- Error - while doing sys__exit\n");
}
#endif

/**
 * @brief sys_getpid, used to return the process pid
 * 
 * @param retval used to return the pid
 * 
 * @return an error in case of failure or 0 in case of success
 */
#if OPT_SHELL
int sys_getpid(pid_t *retval) {
    KASSERT(curproc != NULL);

    /* return the pid of the curr proc */
    *retval = curproc->p_pid;

    return 0;   
}
#endif

/**
 * @brief sys_waitpid, used to wait for the process with the specified pid
 * 
 * @param pid used to specify the process' pid to wait
 * @param status used as exit status of the waitpid
 * @param options used to specify the options
 * @param retval used to return the pid
 * 
 * @return an error in case of failure or a 0 in case of success
 */
#if OPT_SHELL
int sys_waitpid(pid_t pid, int *status, int options, int *retval) {
    KASSERT(curproc != NULL);

    /* checking if pid and status are valid */
    if (pid == curproc->p_pid) {
        return ECHILD;
    } else if (status == NULL) {
        *retval = pid;
        return 0;
    } else if ((int) status == 0x40000000 || (unsigned int) status == (unsigned int) 0x80000000) {
        return EFAULT;
    } else if ((int) status % 4 != 0) {
        return EFAULT;
    }
    /* checking if the child exists, by his pid */
    else if(is_child(curproc, pid)==-1){
        return ECHILD;
    }

    switch (options) {
        case 0:
        break;
        
        case WNOHANG:
            *status = 0;
            *retval = pid;
            return 0;
        break;

        default:
            return EINVAL;
    }

    /* getting the process by pid */
    struct proc *proc = proc_search_pid(pid);
    if (proc == NULL) {
        return ESRCH;
    }

    /* setting the return values */
    if (proc->p_numthreads == 0) {
        *status = proc->p_status;
        *retval = proc->p_pid;
        proc_destroy(proc);
        return 0;
    }

    /* waiting the termination of the process */
    lock_acquire(proc->p_locklock);
    cv_wait(proc->p_cv, proc->p_locklock);
    lock_release(proc->p_locklock);

    /* setting the return values */
    *status = proc->p_status;
    *retval = proc->p_pid;
    if (status == NULL) {
        return EFAULT;
    }

    proc_destroy(proc);
    return 0;
}
#endif


/**
 * @brief sys_fork, used to create a new process, from the existing ones
 * 
 * @param ctf trapframe of the process
 * @param retval used to return the pid of the new created process
 * 
 * @return an error in case of failure or a 0 in case of success
 */
#if OPT_SHELL
int sys_fork(struct trapframe *ctf, pid_t *retval) {
    KASSERT(curproc != NULL);

    /* getting a new valid pid */
    int i = get_valid_pid();
    if (i <= 0) {
        return ENPROC;
    }

    /* creating a new process */
    struct proc *newproc = proc_create_runprogram(curproc->p_name);
    if (newproc == NULL) {
        return ENOMEM;
    }

    /* copying the address space into new process */
    int err = as_copy(curproc->p_addrspace, &(newproc->p_addrspace));
    if (err) {
        proc_destroy(newproc);
        return err;
    }

    /* moving the parent's trapframe */
    struct trapframe *tf_child = (struct trapframe *) kmalloc(sizeof(struct trapframe));
    if(tf_child == NULL){
        proc_destroy(newproc);
        return ENOMEM; 
    }
    memmove(tf_child, ctf, sizeof(struct trapframe));

    struct proc *father=curproc;

    /* adding the new process created to the father (the current process)*/
    if(add_new_child(father, newproc->p_pid)==-1){
        proc_destroy(newproc);
        return ENOMEM; 
    }

    /* linking the child to the father */
    newproc->father_pid=father->p_pid;

    /* adding the new process into the process table */
    err = add_newp((pid_t) i, newproc);
    if (err == -1) {
        return ENOMEM;
    }

    /* calling the thread_fork */
    err = thread_fork(
        curthread->t_name,                  /* same name as the parent  */
        newproc,                            /* newly created process    */      
        call_enter_forked_process,          /* routine to start         */
        (void *) tf_child,                  /* child trapframe          */
        (unsigned long) 0                   
    );

    if (err) {
        proc_destroy(newproc);
        kfree(tf_child);
        return err;
    }

    *retval = newproc->p_pid;
    return 0;
}
#endif

/**
 * @brief sys_execv, used to replace the currently executing program with a new ones
 * 
 * @param pathname used to identify the program to run
 * @param argv an array of string
 * 
 * @return an error in case of failure or a 0 in case of success
 */
#if OPT_SHELL
int sys_execv(const char *pathname, char *argv[]) {
	KASSERT(curproc != NULL);

    userptr_t prog = (userptr_t) pathname;
    userptr_t uargv = (userptr_t) argv;
	
	vaddr_t entrypoint, stackptr;
	int argc;
	int err;

	/* allocating space for pathname in the kernel side */
	char *kpath = (char *) kmalloc(PATH_MAX * sizeof(char));
	if (kpath == NULL) {
		return ENOMEM;
	}

	/* copying the pathname into the allocated space */
	err = copyinstr(prog, kpath, PATH_MAX, NULL);
	if (err) {
		kfree(kpath);
		return err;
	}

	/* copying argv into the kernel side */
	argbuf_t kargv; 
	argbuf_init(&kargv);	
	err = argbuf_fromuser(&kargv, uargv);
	if (err) {
		argbuf_cleanup(&kargv);
		kfree(kpath);
		return err;
	}

	err = loadexec(kpath, &entrypoint, &stackptr);
	if (err) {
		argbuf_cleanup(&kargv);
		kfree(kpath);
		return err;
	}

	kfree(kpath);

	/* copying the argv into process side (user) */
	err = argbuf_copyout(&kargv, &stackptr, &argc, &uargv);
	if (err) {
		panic("- Error - while doing execv with the argbuf_copyout: %s\n", strerror(err));
	}

	argbuf_cleanup(&kargv);

	enter_new_process(argc, uargv, NULL /*uenv*/, stackptr, entrypoint);

	panic("enter_new_process returned\n");
	return EINVAL;
}
#endif
