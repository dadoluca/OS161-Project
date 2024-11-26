#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <syscall.h>

#if OPT_SHELL
#include <synch.h>
#include <kern/fcntl.h>
#include <vfs.h>

#define MAX_PROC 100
static struct _processTable {
  int active;           /* initial value 0 */
  struct proc *proc[MAX_PROC+1]; /* [0] not used. pids are >= 1 */
  int last_i;           /* index of last allocated pid */
  struct spinlock lk;  /* Lock for this table */
} processTable;

#endif
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * Function to remove a child from the parent children list
 */
static int proc_remove_proc(struct proc *this, struct proc *father) {
    if (father == NULL || this == NULL) {
        return -1; //Error
    }

    // Get the father lock to ensure synchronization
    spinlock_acquire(&father->p_lock);

    struct child_node *current = father->p_children_list;
    struct child_node *previous = NULL;

    while (current != NULL) {
        if (current->p == this) {
            if (previous == NULL) {
                // It's the first one in the list
                father->p_children_list = current->next;
            } else {
                previous->next = current->next;
            }
            kfree(current);
            spinlock_release(&father->p_lock);
            return 0;
        }
        previous = current;
        current = current->next;
    }
    spinlock_release(&father->p_lock);

    // Cild not found, error
    return -1;
}

/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
struct proc *
proc_search_pid(pid_t pid) {
#if OPT_SHELL
  struct proc *p;
  if(pid>MAX_PROC){
	  return NULL;
  }
  KASSERT(pid>=0&&pid<MAX_PROC);
  p = processTable.proc[pid];
  KASSERT(p->p_pid==pid);
  return p;
#else
  (void)pid;
  return NULL;
#endif
}

/*
 * G.Cabodi - 2019
 * Initialize support for pid/waitpid.
 */
static void
proc_init_waitpid(struct proc *proc, const char *name) {
#if OPT_SHELL
  /* search a free index in table using a circular strategy */
  int i;
  spinlock_acquire(&processTable.lk);
  i = processTable.last_i+1;
  proc->p_pid = 0;
  if (i>MAX_PROC) i=1;
  while (i!=processTable.last_i) {
    if (processTable.proc[i] == NULL) {
      processTable.proc[i] = proc;
      processTable.last_i = i;
      proc->p_pid = i;
      break;
    }
    i++;
    if (i>MAX_PROC) i=1;
  }
  spinlock_release(&processTable.lk);
  if (proc->p_pid==0) {
    panic("too many processes. proc table is full\n");
  }
  proc->p_status = 0;
#if USE_SEMAPHORE_FOR_WAITPID
  proc->p_sem = sem_create(name, 0);
#else
  proc->p_cv = cv_create(name);
  proc->p_lock = lock_create(name);
#endif
#else
  (void)proc;
  (void)name;
#endif
}

/*
 * Verify if there's a pid available for a new process to be created
 */
#if OPT_SHELL
int proc_verify_pid(){
  int i,pid=0;
  spinlock_acquire(&processTable.lk);
  i = processTable.last_i+1;
  if (i>MAX_PROC) i=1;
  while (i!=processTable.last_i) {
    if (processTable.proc[i] == NULL) {
    	//No assignment needed
      	pid = i;
      	break;
    }
    i++;
    if (i>MAX_PROC) i=1;
  }
  spinlock_release(&processTable.lk);
  if (pid==0) {
    return -1;
  }
  return 0;
}
#endif

/*
 * G.Cabodi - 2019
 * Terminate support for pid/waitpid.
 */
static void
proc_end_waitpid(struct proc *proc) {
#if OPT_SHELL
  /* remove the process from the table */
  int i;
  spinlock_acquire(&processTable.lk);
  i = proc->p_pid;
  KASSERT(i>0 && i<=MAX_PROC);
  processTable.proc[i] = NULL;
  spinlock_release(&processTable.lk);

#if USE_SEMAPHORE_FOR_WAITPID
  sem_destroy(proc->p_sem);
#else
  cv_destroy(proc->p_cv);
  lock_destroy(proc->p_lock);
#endif
#else
  (void)proc;
#endif
}

/*
 *General purpose function used to initialize stdin,stdout and stderr to point to con:
 */
#if OPT_SHELL
static int std_init(struct proc *proc, int fd, int mode) {
	char *con = kstrdup("con:");
	if (con == NULL) {
		return -1;
	}

	/* Allocation of struct openfile */
	proc->fileTable[fd] = (struct openfile *) kmalloc(sizeof(struct openfile));
	if (proc->fileTable[fd] == NULL) {
		kfree(con);
		return -1;
	}

	/* Opening console device */
	int err = vfs_open(con, mode, 0644, &proc->fileTable[fd]->vn);
	if (err) {
		kfree(con);
		kfree(proc->fileTable[fd]);
		return -1;
	}
	kfree(con);

	/* Values initialization */
	proc->fileTable[fd]->offset = 0;
	proc->fileTable[fd]->lock = lock_create("std");
	if (proc->fileTable[fd]->lock == NULL) {
		vfs_close(proc->fileTable[fd]->vn);
		kfree(proc->fileTable[fd]);
		return -1;
	}
	proc->fileTable[fd]->countRef = 1;
	proc->fileTable[fd]->mode = mode;

	return 0;
}
#endif

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
  struct proc *proc;

  proc = kmalloc(sizeof(*proc));
  if (proc == NULL) {
    return NULL;
  }
  proc->p_name = kstrdup(name);
  if (proc->p_name == NULL) {
    kfree(proc);
    return NULL;
  }

  proc->p_numthreads = 0;
  spinlock_init(&proc->p_lock);
	proc->p_thread_list = NULL; // Initialization of the thread list to NULL
	proc->p_children_list = NULL; // Initialization of the children list to NULL
	proc->p_father_proc = NULL; // Initialization of the father to NULL

  /* VM fields */
  proc->p_addrspace = NULL;

  /* VFS fields */
  proc->p_cwd = NULL;
  proc->p_terminated = 0; // Initialize it to zero, it is set to 1 once the process terminates
  proc_init_waitpid(proc,name);
#if OPT_SHELL
	bzero(proc->fileTable,OPEN_MAX*sizeof(struct openfile *));
	//It's not possible to initialize stdin,stdout,stderr here
	//It would do it for the kernel process also
#endif
  return proc;
}

#if OPT_SHELL
/*
 * Check if the process identified by pid is a child of the process calling waitpid
 */
int check_is_child(pid_t pid){
	struct proc *p= proc_search_pid(pid); //get the proc data structure for the process with pid=pid
	if(p==NULL){ //if not found, it returns -1
		return -1;
	}
	if(p->p_father_proc==curproc){
		return 1;
	}
	return 0;
}
#endif
#if OPT_SHELL
/*
 * Check if a process has a terminated child in its children list
 */
struct proc * check_is_terminated(struct proc *p){
	struct child_node *current = p->p_children_list;
	while(current!=NULL){
		if(current->p->p_terminated==1){
			return current->p;
		}
		//Go on with the list
		current = current->next;
	}
	return NULL;
}
#endif

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
  /*
   * You probably want to destroy and null out much of the
   * process (particularly the address space) at exit time if
   * your wait/exit design calls for the process structure to
   * hang around beyond process exit. Some wait/exit designs
   * do, some don't.
   */

  KASSERT(proc != NULL);
  KASSERT(proc != kproc);

  /*
   * We don't take p_lock in here because we must have the only
   * reference to this structure. (Otherwise it would be
   * incorrect to destroy it.)
   */

  /* VFS fields */
  if (proc->p_cwd) {
    VOP_DECREF(proc->p_cwd);
    proc->p_cwd = NULL;
  }
  /* VM fields */
  if (proc->p_addrspace) {
    /*
     * If p is the current process, remove it safely from
     * p_addrspace before destroying it. This makes sure
     * we don't try to activate the address space while
     * it's being destroyed.
     *
     * Also explicitly deactivate, because setting the
     * address space to NULL won't necessarily do that.
     *
     * (When the address space is NULL, it means the
     * process is kernel-only; in that case it is normally
     * ok if the MMU and MMU- related data structures
     * still refer to the address space of the last
     * process that had one. Then you save work if that
     * process is the next one to run, which isn't
     * uncommon. However, here we're going to destroy the
     * address space, so we need to make sure that nothing
     * in the VM system still refers to it.)
     *
     * The call to as_deactivate() must come after we
     * clear the address space, or a timer interrupt might
     * reactivate the old address space again behind our
     * back.
     *
     * If p is not the current process, still remove it
     * from p_addrspace before destroying it as a
     * precaution. Note that if p is not the current
     * process, in order to be here p must either have
     * never run (e.g. cleaning up after fork failed) or
     * have finished running and exited. It is quite
     * incorrect to destroy the proc structure of some
     * random other process while it's still running...
     */
    struct addrspace *as;

    if (proc == curproc) {
      as = proc_setas(NULL);
      as_deactivate();
    }
    else {
      as = proc->p_addrspace;
      proc->p_addrspace = NULL;
    }
    as_destroy(as);
  }
  //The list of children has to be deallocated
#if OPT_SHELL
	while(proc->p_children_list!=NULL){
		struct child_node *curNode = proc->p_children_list;
		struct child_node *nextOne = curNode->next; //The last one will point to null and the while won't enter

		//call proc_destroy recursively on the child process
		proc_destroy(curNode->p);
		proc->p_children_list=nextOne; //go on in the list

		kfree(curNode); //free the child_node structure
	}
	if(proc->p_father_proc != NULL){
		//remove the child process from the list of children of the father
		if(proc_remove_proc(proc,proc->p_father_proc)!=0){
			panic("The child should exist in the father data structure");
		}
	}
#endif 

  KASSERT(proc->p_numthreads == 0);

  proc_end_waitpid(proc);

  kfree(proc->p_name);
  kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
  kproc = proc_create("[kernel]");
  if (kproc == NULL) {
    panic("proc_create for kproc failed\n");
  }
#if OPT_SHELL
  spinlock_init(&processTable.lk);
  /* kernel process is not registered in the table */
  processTable.active = 1;
#endif
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
  struct proc *newproc;

  newproc = proc_create(name);
  if (newproc == NULL) {
    return NULL;
  }

  /* VM fields */

  newproc->p_addrspace = NULL;

  /*Initialization of stdin,stdout and stderr to point to the console device*/
	if (std_init( newproc, 0, O_RDONLY) == -1) {
		return NULL;
	} else if (std_init(newproc, 1, O_WRONLY) == -1) {
		return NULL;
	} else if (std_init(newproc, 2, O_WRONLY) == -1) {
		return NULL;
	}

  /* VFS fields */

  /*
   * Lock the current process to copy its current directory.
   * (We don't need to lock the new process, though, as we have
   * the only reference to it.)
   */
  spinlock_acquire(&curproc->p_lock);
  if (curproc->p_cwd != NULL) {
    VOP_INCREF(curproc->p_cwd);
    newproc->p_cwd = curproc->p_cwd;
  }
  spinlock_release(&curproc->p_lock);

  return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
  int spl;

  KASSERT(t->t_proc == NULL);

  spinlock_acquire(&proc->p_lock);
  proc->p_numthreads++;
  spinlock_release(&proc->p_lock);

  spl = splhigh();
  t->t_proc = proc;
  splx(spl);

  return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
  struct proc *proc;
  int spl;

  proc = t->t_proc;
  KASSERT(proc != NULL);
  spinlock_acquire(&proc->p_lock);
  KASSERT(proc->p_numthreads > 0);
  proc->p_numthreads--;
  spinlock_release(&proc->p_lock);

  spl = splhigh();
  t->t_proc = NULL;
  splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
  struct addrspace *as;
  struct proc *proc = curproc;

  if (proc == NULL) {
    return NULL;
  }

  spinlock_acquire(&proc->p_lock);
  as = proc->p_addrspace;
  spinlock_release(&proc->p_lock);
  return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
  struct addrspace *oldas;
  struct proc *proc = curproc;

  KASSERT(proc != NULL);

  spinlock_acquire(&proc->p_lock);
  oldas = proc->p_addrspace;
  proc->p_addrspace = newas;
  spinlock_release(&proc->p_lock);
  return oldas;
}



        /* G.Cabodi - 2019 - support for waitpid */
int 
proc_wait(struct proc *proc)
{
#if OPT_SHELL
        int return_status;
        /* NULL and kernel proc forbidden */
  KASSERT(proc != NULL);
  KASSERT(proc != kproc);

        /* wait on semaphore or condition variable */ 
#if USE_SEMAPHORE_FOR_WAITPID
        P(proc->p_sem);
#else
        lock_acquire(proc->p_lock);
        cv_wait(proc->p_cv);
        lock_release(proc->p_lock);
#endif
        return_status = proc->p_status;
        proc_destroy(proc);
        return return_status;
#else
        /* this doesn't synchronize */ 
        (void)proc;
        return 0;
#endif
}

#if OPT_SHELL
/* G.Cabodi - 2019 - support for waitpid */
void
proc_signal_end(struct proc *proc)
{
#if USE_SEMAPHORE_FOR_WAITPID
      V(proc->p_sem);
#else
      lock_acquire(proc->p_lock);
      cv_signal(proc->p_cv);
      lock_release(proc->p_lock);
#endif
}

void 
proc_file_table_copy(struct proc *psrc, struct proc *pdest) {
  int fd;
  for (fd=0; fd<OPEN_MAX; fd++) {
    struct openfile *of = psrc->fileTable[fd];
    pdest->fileTable[fd] = of;
    if (of != NULL) {
      /* incr reference count */
      openfileIncrRefCount(of);
    }
  }
}
#endif