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
  bool active;           /* initial value 0 */
  struct proc *proc[MAX_PROC+1]; /* [0] not used. pids are >= 1 */
  int last_pid;           /* index of last allocated pid */
  struct spinlock lk;  /* Lock for this table */
} processTable;

#endif
/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;


/*
 * Verify if there's a pid available for a new process to be created
 */
#if OPT_SHELL
int get_valid_pid(void){
  /* CHECKING SPACE AVAILABILITY IN PROCESS TABLE WITH CIRCULAR BUFFER */
	int index = (processTable.last_pid + 1 > MAX_PROC) ? 1 : processTable.last_pid + 1;
	while (index != processTable.last_pid) {
        if (processTable.proc[index] == NULL) {
            break;
        }

        index ++;
        index = (index > MAX_PROC) ? 1 : index;
    }

	/* POSITION NOT FOUND */
	if (index == processTable.last_pid) {
        return -1; 
    }

	/* POSITION FOUND */
	return index;
}
#endif

#if OPT_SHELL
int add_newp(pid_t pid, struct proc *proc) {

	/* EVALUATING PARAMETERS */
	if (pid <= 0 || pid > MAX_PROC +1 || proc == NULL) {
		return -1;
	}

	/* ADDING PROCESS */
	spinlock_acquire(&processTable.lk);
	processTable.proc[pid] = proc;

	/* UPDATE LAST PID POSITION */
	processTable.last_pid = pid;
	spinlock_release(&processTable.lk);

	/* TASK COMPLETED SUCCESSFULLY */
	return 0;
}
#endif

#if OPT_SHELL
void remove_proc(pid_t pid) {

	/* REMOVING PROCESS */
	spinlock_acquire(&processTable.lk);
	processTable.proc[pid] = NULL;
	spinlock_release(&processTable.lk);


}
#endif

#if OPT_SHELL
void call_enter_forked_process(void *tfv, unsigned long dummy) {

	(void) dummy;

	/* CALLING BUILT-IN FUNCTION */
	struct trapframe *tf = (struct trapframe *) tfv;
	enter_forked_process(tf); 

	/* SHOULD NOT GET HERE */
	panic("[!] enter_forked_process() returned unexpectedly\n");
}
#endif

#if OPT_SHELL
struct proc *proc_search_pid(pid_t pid) {

	/* CHECKING PID CONSTRAINTS */
	if (pid <= 0 || pid > MAX_PROC) {
		return NULL;
	}

	/* RETRIEVING PROCESS BASED ON THE INDEX PID */
	struct proc *proc = processTable.proc[pid];
	if (proc->p_pid != pid) {
		return NULL;
	}

	/* TASK COMPLETED SUCCESSFULLY */
	return proc;
}
#endif

#if OPT_SHELL
static int std_init(struct proc *proc, int fd, int mode) {

	/* ASSIGNMENT OF THE CONSOLE NAME */
	char *con = kstrdup("con:");
	if (con == NULL) {
		return -1;
	}

	/* ALLOCATING SPACE IN THE FILETABLE */
	proc->fileTable[fd] = (struct openfile *) kmalloc(sizeof(struct openfile));
	if (proc->fileTable[fd] == NULL) {
		kfree(con);
		return -1;
	}

	/* OPENING ASSOCIATED FILE */
	int err = vfs_open(con, mode, 0644, &proc->fileTable[fd]->vn);
	if (err) {
		kfree(con);
		kfree(proc->fileTable[fd]);
		return -1;
	}
	kfree(con);

	/* INITIALIZATION OF VALUES */
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

#if OPT_SHELL
static int proc_init(struct proc *proc, const char *name) {

	/* ACQUIRING THE SPINLOCK */
	spinlock_acquire(&processTable.lk);
	proc->p_pid = -1;

	/* SEARCH FREE INDEX IN THE TABLE USING CIRCULAR STRATEGY */
	int index = processTable.last_pid + 1;
	index = (index > MAX_PROC) ? 1 : index;		// skipping [0] (kernel process)
	while (index != processTable.last_pid) {
		if (processTable.proc[index] == NULL) {
			processTable.proc[index] = proc;
			processTable.last_pid = index;
			proc->p_pid = index;
			break;
		}
		index++;
		index = (index > MAX_PROC) ? 1 : index;
	}

	/* RELEASING THE SPINLOCK */
	spinlock_release(&processTable.lk);
	if (proc->p_pid <= 0) {
		return proc->p_pid;
	}

	/* PROCESS STATUS INITIALIZATION */
	proc->p_status = 0;

	/*SETTING FATHER PID AS -1*/
	/*FOR THE FIRST PROCESS IT WILL NOT BE CHANGED*/
	proc->father_pid=-1;

	/*PROCESS CHILDREN LIST INITIALIZATION*/
	proc->child_list= NULL;

	/* PROCESS CV AND LOCK INITIALIZATION */
	proc->p_cv = cv_create(name);
  	proc->p_locklock = lock_create(name);
	if (proc->p_cv == NULL || proc->p_locklock == NULL) {
		return -1;
	}

	/* TASK COMPLETED SUCCESSFULLY */
	return proc->p_pid;
}
#endif

static int proc_deinit(struct proc *proc) {
#if OPT_SHELL
	struct proc* parent_proc;
	/* ACQUIRING THE SPINLOCK */
	spinlock_acquire(&processTable.lk);

	/* ACQUIRING PROCESS PID */
	int index = proc->p_pid;
	if (index <= 0 || index > MAX_PROC) {
		return -1;
	}

	/* RELEASING ENTRY IN PROCESS TABLE */
	processTable.proc[index] = NULL;

	/* PROCESS CV AND LOCK DESTROY */
	cv_destroy(proc->p_cv);
  	lock_destroy(proc->p_locklock);

	/* RELEASING THE SPINLOCK */
	spinlock_release(&processTable.lk);

	/*DESTROYING THE CHILD LIST AND SETTING CHILDREN AS ORPHANS*/
	if(delete_child_list(proc)==-1)
		return -1;
	
	/*REMOVING THE PROCESS FROM THE CHILD LIST OF ITS PARENT*/
	if(proc->father_pid!=-1){
		parent_proc=proc_search_pid(proc->father_pid);
		if(proc->father_pid==kproc->p_pid)
			parent_proc=kproc;			
		if(parent_proc==NULL)
			return -1;

		if((remove_child_from_list(parent_proc, proc->p_pid)==-1))
			return -1;
	}

	/* TASK COMPLETED SUCCESSFULLY */
	return 0;
#endif
}

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

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

#if OPT_SHELL

	/**
	 * @brief Zeroing out the block of memory used by the process fileTable (i.e.
	 * 		  initializing the struct).
	 */
	bzero(proc->fileTable, OPEN_MAX * sizeof(struct openfile*));

	/* ADD PROCESS TO THE PROCESS TABLE */
	if (strcmp(name, "[kernel]") != 0 && proc_init(proc, name) <= 0) {
		kfree(proc);
		return NULL;
	}
#endif

	return proc;
}

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

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

#if OPT_SHELL
	if (proc_deinit(proc) != 0) {
		panic("[ERROR] some errors occurred in the management of the process table\n");
	}
#endif

	kfree(proc->p_name);
	kfree(proc);
}

void
proc_bootstrap(void)
{

	/* KERNEL PROCESS INITIALIZATION AND CREATION */
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}

	/* USER PROCESS INITIALIZATION (TABLE) */
#if OPT_SHELL
	spinlock_init(&processTable.lk);	/* lock initialization 								*/
	processTable.proc[0] = kproc;		/* registering kernel process in the process table 	*/
	KASSERT(processTable.proc[0] != NULL);
	for (int i = 1; i <= MAX_PROC; i++) {
		processTable.proc[i] = NULL;
	}
	processTable.active = true;		/* activating the process table 					*/
	processTable.last_pid = 0;			/* last used PID 									*/
#endif
}

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

	/* VFS fields */

#if OPT_SHELL
	/* CONSOLE INITIALIZATION FOR STDIN, STDOUT AND STDERR */
	if (std_init(newproc, 0, O_RDONLY) == -1) {
		return NULL;
	} else if (std_init(newproc, 1, O_WRONLY) == -1) {
		return NULL;
	} else if (std_init(newproc, 2, O_WRONLY) == -1) {
		return NULL;
	}
#endif

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

#if OPT_SHELL
int add_new_child(struct proc* proc, pid_t child_pid){
	struct child_list* app=proc->child_list;

	if(proc->child_list==NULL){
		proc->child_list=(struct child_list *) kmalloc(sizeof(struct child_list));
		if(proc->child_list==NULL)
			return -1;
		proc->child_list->next=NULL;
		proc->child_list->pid=child_pid;
		return 0;
	}
		

	while(app->next!=NULL){
		app=app->next;
	}

	app->next=(struct child_list *) kmalloc(sizeof(struct child_list));
	if(app->next==NULL)
		return -1;
	app->next->next=NULL;
	app->next->pid=child_pid;
	return 0;
}
#endif

#if OPT_SHELL
int delete_child_list(struct proc* proc){
	struct child_list* app=proc->child_list;
	struct proc* child_proc;


	while(app!=NULL){
		proc->child_list=app->next;

		/*FINDING THE CHILD STRUCTURE*/
		child_proc=proc_search_pid(app->pid);
		if(child_proc==NULL)
			return -1;

		/*SETTING THE PARENT PID AS -1*/	
		child_proc->father_pid=-1;

		/*REMOVING THE CHILD*/
		app->next=NULL;
		kfree(app);

		app=proc->child_list;
	}
	
	return 0;
}
#endif

#if OPT_SHELL
int remove_child_from_list(struct proc* proc, pid_t child_pid){
	struct child_list* app=proc->child_list;
	struct child_list* prev_child=NULL;


	while(app!=NULL){
		if(app->pid==child_pid){
			if(prev_child==NULL)
				proc->child_list=app->next;
			else
				prev_child->next=app->next;
			kfree(app);
			return 0;
		}
		prev_child=app;
		app=app->next;
	}
	
	return -1;
}
#endif

#if OPT_SHELL
int is_child(struct proc* proc, pid_t child_pid){
	struct child_list* app=proc->child_list;


	while(app!=NULL){
		if(app->pid==child_pid){
			return 0;
		}
		app=app->next;
	}
	
	return -1;
}
#endif
