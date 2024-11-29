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

/* system open file table */
#if OPT_SHELL
struct openfile {
  struct vnode *vn;
  off_t offset;
  int mode;
  unsigned int countRef;
  struct lock *lock;
};
#endif

/**
 * @brief child_list, used to keep track of the child of a process
 */
#if OPT_SHELL
struct child_list{
	pid_t pid;
	struct child_list *next;
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
  int p_status;                   /* status as obtained by exit() */
  pid_t p_pid;                    /* process pid */
	struct child_list *child_list; //head of the list of children for a process, kept to terminate them on exit
  pid_t father_pid;
  struct cv *p_cv;
  struct lock *p_locklock;
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

#if OPT_SHELL
int get_valid_pid(void);
int add_newp(pid_t pid, struct proc *proc);
void remove_proc(pid_t pid);
void call_enter_forked_process(void *tfv, unsigned long dummy);
struct proc *proc_search_pid(pid_t pid);
int add_new_child(struct proc* proc, pid_t child_pid);
int delete_child_list(struct proc* proc);
int remove_child_from_list(struct proc* proc, pid_t child_pid);
int is_child(struct proc* proc, pid_t child_pid);
#endif

#endif /* _PROC_H_ */