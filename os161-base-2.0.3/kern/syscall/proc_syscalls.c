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

/*
 * system calls for process management
 */
void
sys__exit(int status)
{
  /* RETRIEVING STATUS OF THE CURRENT PROCESS */
    struct proc *proc = curproc;
    proc->p_status = _MKWAIT_EXIT(status);    /* exitcode & 0xff */

    /* REMOVING THREAD BEFORE SIGNALLING DUE TO RACE CONDITIONS */
    proc_remthread(curthread);        /* remove thread from current process */

    /* SIGNALLING THE TERMINATION OF THE PROCESS */
    lock_acquire(proc->p_locklock);
    cv_signal(proc->p_cv, proc->p_locklock);
    lock_release(proc->p_locklock);

    /* MAIN THREAD TERMINATES HERE. BYE BYE */
    thread_exit();

    /* WAIT! YOU SHOULD NOT HAPPEN TO BE HERE */
    panic("[!] Wait! You should not be here. Some errors happened during thread_exit()...\n");
}

#if OPT_SHELL

int sys_waitpid(pid_t pid, int *status, int options, int *retval)
{
  /* SOME ASSERTIONS */
    KASSERT(curproc != NULL);

    /* CHECKING ARGUMENTS */
    if (pid == curproc->p_pid) {
        return ECHILD;
    } else if (status == NULL) {
        *retval = pid;
        return 0;
    }
    /* TEMPORARY */
    else if ((int) status == 0x40000000 || (unsigned int) status == (unsigned int) 0x80000000) {
        return EFAULT;
    } else if ((int) status % 4 != 0) {
        return EFAULT;
    }
    /*CHECKING IF TRYING TO WAIT FOR A NON-CHILD PROCESS*/
    else if(is_child(curproc, pid)==-1){
        return ECHILD;
    }

    /* OPTIONS */
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

    /* RETRIEVING PROCESS */
    struct proc *proc = proc_search_pid(pid);
    if (proc == NULL) {
        return ESRCH;
    }

    if (proc->p_numthreads == 0) {
        *status = proc->p_status;
        *retval = proc->p_pid;
        proc_destroy(proc);
        return 0;
    }

    /* WAITING TERMINATION OF THE PROCESS */
    lock_acquire(proc->p_locklock);
    cv_wait(proc->p_cv, proc->p_locklock);
    lock_release(proc->p_locklock);

    /* ASSIGNING RETURN STATUS */
    *status = proc->p_status;
    *retval = proc->p_pid;
    if (status == NULL) {
        return EFAULT;
    }

    /* TASK COMPLETED SUCCESSFULLY */
    proc_destroy(proc);
    return 0;
}
#endif

#if OPT_SHELL
int sys_getpid(pid_t *retval) {
    KASSERT(curproc != NULL); /*check that the currproc exists */
    /* return the pid of the curr proc. it cant never fail*/
    *retval = curproc->p_pid;
    return 0;   
}
#endif


#if OPT_SHELL
int sys_fork(struct trapframe *ctf, pid_t *retval) {

    /* ASSERTING CURRENT PROCESS TO ACTUALLY EXIST */
    KASSERT(curproc != NULL);

    /* CHECKING SPACE AVAILABILITY IN PROCESS TABLE */
    int index = get_valid_pid();
    if (index <= 0) {
        return ENPROC;  /* There are already too many processes on the system. */
    }

    /* CREATING NEW RUNNABLE PROCESS */
    struct proc *newproc = proc_create_runprogram(curproc->p_name);
    if (newproc == NULL) {
        return ENOMEM;  /* Sufficient virtual memory for the new process was not available. */
    }

    /* COPYING ADDRESS SPACE */
    int err = as_copy(curproc->p_addrspace, &(newproc->p_addrspace));
    if (err) {
        proc_destroy(newproc);
        return err;
    }

    /* COPYING PARENT'S TRAPFRAME */
    struct trapframe *tf_child = (struct trapframe *) kmalloc(sizeof(struct trapframe));
    if(tf_child == NULL){
        proc_destroy(newproc);
        return ENOMEM; 
    }
    memmove(tf_child, ctf, sizeof(struct trapframe));

    /* TO BE DONE: linking parent/child, so that child terminated on parent exit */
    /*DEBUGGING PURPOSE*/
    struct proc *father=curproc;

    /*ADDING NEW CHILD TO FATHER*/
    if(add_new_child(father, newproc->p_pid)==-1){
        proc_destroy(newproc);
        return ENOMEM; 
    }


    /*LINKING CHILD TO FATHER*/
    newproc->father_pid=father->p_pid;

    /* ADDING NEW PROCESS TO THE PROCESS TABLE */
    err = add_newp((pid_t) index, newproc);
    if (err == -1) {
        return ENOMEM;
    }

    /* CALLING THREAD FORK() AND START NEW THREAD ROUTINE */
    err = thread_fork(
        curthread->t_name,                  /* same name as the parent  */
        newproc,                            /* newly created process    */      
        call_enter_forked_process,          /* routine to start         */
        (void *) tf_child,                  /* child trapframe          */
        (unsigned long) 0                   /* unused                   */
    );

    if (err) {
        proc_destroy(newproc);
        kfree(tf_child);
        return err;
    }

    /* TASK COMPLETED SUCCESSFULLY */
    *retval = newproc->p_pid;      // parent return pid of child
    return 0;
}
#endif
