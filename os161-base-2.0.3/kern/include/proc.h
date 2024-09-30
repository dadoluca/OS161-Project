#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <spinlock.h>
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
/* G.Cabodi - 2019 - implement waitpid: 
   synch with semaphore (1) or cond.var.(0) */
#define USE_SEMAPHORE_FOR_WAITPID 1
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
        /* G.Cabodi - 2019 - implement waitpid: synchro, and exit status */
        int p_status;                   /* status as obtained by exit() */
        pid_t p_pid;                    /* process pid */
#if USE_SEMAPHORE_FOR_WAITPID
  struct semaphore *p_sem;
#else
        struct cv *p_cv;
        struct lock *p_lock;
#endif
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

#endif /* _PROC_H_ */