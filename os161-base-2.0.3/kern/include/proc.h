#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <spinlock.h>
#include <limits.h>
#include "opt-shell.h"

struct addrspace;
struct thread;
struct vnode;

/*
 * Process structure.
 *
 * Note that we only count the number of threads in each process.
 * (And, unless you implement multithreaded user processes, this
 * number will not exceed 1 except in kproc.) If you want to know
 * exactly which threads are in the process, e.g. for debugging, add
 * an array and a sleeplock to protect it. (You can't use a spinlock
 * to protect an array because arrays need to be able to call
 * kmalloc.)
 *
 * You will most likely be adding stuff to this structure, so you may
 * find you need a sleeplock in here for other reasons as well.
 * However, note that p_addrspace must be protected by a spinlock:
 * thread_switch needs to be able to fetch the current address space
 * without sleeping.
 */

#if OPT_SHELL
#define USE_SEMAPHORE_FOR_WAITPID 1
#endif

#if OPT_SHELL
/* system open file table */
struct openfile {
  struct vnode *vn;
  off_t offset;
  int mode;
  unsigned int countRef;
  struct lock *lock;
};
#endif

#if OPT_SHELL
struct thread_node{ //It's used in order to know the threads belonging to a process, so that we can terminate them all in case a bad instruction is hit 
	struct thread *t;
	struct thread_node *next;
};
struct child_node{ //To keep track of childs of the process
	struct proc *p;
	struct child_node *next;
};
#endif 

struct proc {
  char *p_name;      /* Name of this process */
  struct spinlock p_lock;    /* Lock for this structure */
  unsigned p_numthreads;    /* Number of threads in this process */

  /* VM */
  struct addrspace *p_addrspace;  /* virtual address space */

  /* VFS */
  struct vnode *p_cwd;    /* current working directory */

  /* add more material here as needed */
#if OPT_SHELL
        struct thread_node *p_thread_list; //head of the list of threads 
		    struct child_node *p_children_list; //head of the list of children for a process, kept to terminate them on exit
		    struct proc *p_father_proc; //pointer to the proc structure of the father (if any)
        int p_status;                   /* status as obtained by exit() */
        pid_t p_pid;                    /* process pid */
        int p_terminated; //added to verify if a process terminated
#if USE_SEMAPHORE_FOR_WAITPID
  struct semaphore *p_sem;
#else
        struct cv *p_cv;
        struct lock *p_lock;
#endif
  struct openfile *fileTable[OPEN_MAX];
#endif
};

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);

/* wait for process termination, and return exit status */
int proc_wait(struct proc *proc);
/* get proc from pid */
struct proc *proc_search_pid(pid_t pid);
/* signal end/exit of process */
void proc_signal_end(struct proc *proc);
void proc_file_table_copy(struct proc *psrc, struct proc *pdest);
int check_is_child(pid_t pid);
int proc_verify_pid(void);
struct proc * check_is_terminated(struct proc *p);

#endif /* _PROC_H_ */