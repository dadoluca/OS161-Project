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

int
sys_waitpid(pid_t pid, userptr_t statusp, int options, int *retval)
{
#if OPT_SHELL
  /*pid can be >0, -1 or <-1. The latter case is not considered because it references the group id (not handled)
      pid = -1 should wait for any of its child 
      this means that the pid is constrained to be >0*/

  if (pid <= 0) { 
      *retval = ENOSYS;
      return -1;
  }
  /*ECHILD is returned if the process hasn't got any unwaiting children*/
  if(curproc->p_children_list==NULL){
    *retval = ECHILD;
    return -1;
  }
  /*Check that statusp is valid to pass badcall tests*/
  if(statusp!=NULL){
    int result;
    int dummy;
    result = copyin((const_userptr_t)statusp, &dummy, sizeof(dummy)); //It's easy to do it through copyin
    if (result) {
        *retval = EFAULT;
        return -1;
    }
  }
  /*The process is allowed to wait only for a process that is its child*/
  int ret = check_is_child(pid);
  /*The process doesn't exist*/
  if (ret == -1) { 
    *retval = ESRCH;
    return -1;
  }
  /*The process is not a child of the calling process*/
  if (ret == 0) { 
    *retval = ECHILD;
    return -1;
  }
  struct proc *p = proc_search_pid(pid);
  switch (options) {
    case 0:
      // No options, standard blocking wait
      break;
    case WNOHANG:{
      /*Check if any of the children of the calling process has terminated. In this case, return its pid and status, otherwise 0*/
      struct proc *p= check_is_terminated(curproc);
      if (p == NULL) {
          return 0;
      }
      /*Otherwise it goes on with p, it performs the wait which is non-blocking, frees the list by the child and destroys the proc data structure*/
      break;}
    /*case WEXITED: { It's not standard
      // Check if the child process has exited
      if (p->p_terminated==1) {
        break; // Exit normally if child has exited
      }
      *retval = ECHILD;
      return -1;
    }*/
    default:{
      *retval=EINVAL; 
      return -1;
    }
  }
  int s = proc_wait(p);
  if (statusp != NULL) {
      // Use a temporary variable to ensure alignment
      int kstatus;
      kstatus = s;
      // Copy the status back to user space
      int result = copyout(&kstatus, statusp, sizeof(kstatus));
      if (result) {
          *retval = EFAULT;
          return -1;
      }
  }
  return pid;
#endif
}

pid_t
sys_getpid(void)
{
#if OPT_SHELL
  KASSERT(curproc != NULL);
  return curproc->p_pid;
#endif
}

#if OPT_SHELL
static void
call_enter_forked_process(void *tfv, unsigned long dummy) {
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