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



#if OPT_SHELL
int sys_execv(const char *progname, char *argv[]) {

    
	/* SOME ASSERTIONS */
	KASSERT(curproc != NULL);

	/* CASTING PARAMETER */
    userptr_t prog = (userptr_t) progname;
    userptr_t uargv = (userptr_t) argv;
	
	vaddr_t entrypoint, stackptr;
	int argc;
	int err;

	/* ALLOCATING SPACE FOR PROGNAME IN KERNEL SIDE */
	char *kpath = (char *) kmalloc(PATH_MAX * sizeof(char));
	if (kpath == NULL) {
		return ENOMEM;
	}

	/* COPYING PROGNAME IN KERNEL SIDE */
	err = copyinstr(prog, kpath, PATH_MAX, NULL);
	if (err) {
		kfree(kpath);
		return err;
	}

	/* COPY ARGV FROM USER SIDE TO KERNEL SIDE */
	argbuf_t kargv; 
	argbuf_init(&kargv);	
	err = argbuf_fromuser(&kargv, uargv);
	if (err) {
		argbuf_cleanup(&kargv);
		kfree(kpath);
		return err;
	}

	/**
	 * LOAD THE EXECUTABLE
	 * NB: must not fail from here on, the old address space has been destroyed
	 * 	   and, therefore, there is nothing to restore in case of failure.
	 */
	err = loadexec(kpath, &entrypoint, &stackptr);
	if (err) {
		argbuf_cleanup(&kargv);
		kfree(kpath);
		return err;
	}

	/* Goodbye kpath, you useless now... */
	kfree(kpath);

	/* COPY ARGV FROM KERNEL SIDE TO PROCESS (USER) SIDE */
	err = argbuf_copyout(&kargv, &stackptr, &argc, &uargv);
	if (err) {
		/* if copyout fails, *we* messed up, so panic */
		panic("execv: copyout_args failed: %s\n", strerror(err));
	}

	/* free the argv buffer space */
	argbuf_cleanup(&kargv);

	/* Warp to user mode. */
	enter_new_process(argc, uargv, NULL /*uenv*/, stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
#endif