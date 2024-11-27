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
#if OPT_SHELL
  struct proc *p = curproc;
  p->p_status = status & 0xff; /* just lower 8 bits returned */
  spinlock_acquire(&p->p_lock);
  p->p_terminated = 1; // Process is terminated
  spinlock_release(&p->p_lock);
  proc_remthread(curthread);
  proc_signal_end(p); // mark the end of the process without destroying it
#else
  /* get address space of current process and destroy */
  struct addrspace *as = proc_getas();
  as_destroy(as);
#endif
  thread_exit();
  panic("thread_exit returned (should not happen)\n");
}

#if OPT_SHELL

int sys_waitpid(pid_t pid, userptr_t status, int options, int *retval)
{
  /*  
    The PID can be greater than 0, -1, or less than -1. The case where the PID is less than -1 is not considered, as it refers to the group ID (which is not handled).
    When PID is -1, it indicates the process should wait for any of its child processes.
    This implies that the PID must be greater than 0.
  */

  /* assign retval=.1 in any case. in order to change it only un success cases */
  *retval = -1;

  if (pid <= 0) { 
    return ENOSYS;
  }

  /* ECHILD return if no children_list is found */
  if(curproc->p_children_list==NULL){
    return ECHILD;
  }
  /* Check that status is valid */
  if(status!=NULL){
    int dummy;

    int result = copyin((const_userptr_t)status, &dummy, sizeof(dummy));

    if (result) {
        return EFAULT;
    }
  }


  /* The process can wait only for its child*/
  int res = check_is_child(pid);

  /*The process doesn't exist*/
  if (res == -1) { 
    return ESRCH;  /* No such process */
  }

  /*The process is not a child of the calling process*/
  if (res == 0) { 
    return ECHILD; /* No child processes */
  }


  struct proc *p = proc_search_pid(pid);
  switch (options) {
    case 0:
      break;
    case WNOHANG:{ /* WNOHANG: Nonblocking. */
      /* Check if any of the children of the calling process has terminated. In this case, return its pid and status, otherwise 0 */
      struct proc *p = check_is_terminated(curproc);
      if (p == NULL) {
          *retval = 0;
          return 0; /* success */
      }
      break;
    }
    
    default:
      return EINVAL; /* invalid argument, we cant manage it */
    
  }
  int s = proc_wait(p);
  if (status != NULL) {
      /* We use a temporary variable in order to ensure alignment */
      int kstatus;
      kstatus = s;

      /* Copy the status back to user space */
      int result = copyout(&kstatus, status, sizeof(kstatus));
      if (result) {
          return EFAULT;
      }
  }

  *retval = pid;
  /* success */
  return 0;

#endif
}

#if OPT_SHELL
int sys_getpid(pid_t *retval) {
    KASSERT(curproc != NULL); /*check that the currproc exists */
    /* return the pid of the curr proc. it cant never fail*/
    *retval = curproc->p_pid;
    return 0;   
}
#endif


#if OPT_SHELL
static void call_enter_forked_process(void *tfv, unsigned long dummy) {
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf); 
  panic("enter_forked_process returned (should not happen)\n");
}


int sys_fork(struct trapframe *ctf, pid_t *retval) {
  struct trapframe *tf_child;
  struct proc *newp;
  int result;

  KASSERT(curproc != NULL);

  if(proc_verify_pid() == -1) {
    return ENPROC;
  }

  newp = proc_create_runprogram(curproc->p_name);
  if (newp == NULL) {
    return ENOMEM;
  }

  /* done here as we need to duplicate the address space 
     of thbe current process */
  as_copy(curproc->p_addrspace, &(newp->p_addrspace));
  if(newp->p_addrspace == NULL){
    proc_destroy(newp); 
    return ENOMEM;
  }

  proc_file_table_copy(newp,curproc);

  /* we need a copy of the parent's trapframe */
  tf_child = kmalloc(sizeof(struct trapframe));
  if(tf_child == NULL){
    proc_destroy(newp);
    return ENOMEM; 
  }
  memcpy(tf_child, ctf, sizeof(struct trapframe));

  /* linking parent/child, so that child terminated 
     on parent exit */
  struct child_node *newChild = kmalloc(sizeof(struct child_node));
  if(newChild == NULL) {
    return ENOMEM;
  }
  // adding the child to the father child's list
  newChild->p = newp;
  newChild->next = curproc->p_children_list;
  curproc->p_children_list = newChild;
  // adding the father to the childre father's list
  newp->p_father_proc = curproc;

  result = thread_fork(
     curthread->t_name, newp,
     call_enter_forked_process, 
     (void *)tf_child, (unsigned long)0/*unused*/);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
    return ENOMEM;
  }

  *retval = newp->p_pid;

  return 0;
}
#endif
