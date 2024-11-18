/*
 * Very simple implementation of sys_read and sys_write.
 * just works (partially) on stdin/stdout
 */

#include <types.h>
#include <kern/unistd.h>
#include <kern/errno.h>
#include <clock.h>
#include <syscall.h>
#include <synch.h>
#include <current.h>
#include <lib.h>
#include <kern/fcntl.h>
#include <kern/stat.h>
#include <copyinout.h>
#include <vnode.h>
#include <vfs.h>
#include <limits.h>
#include <uio.h>
#include <proc.h>
#include <kern/seek.h>
#include <stat.h>
#include <endian.h>

/* max num of system wide open files */
#define SYSTEM_OPEN_MAX (10*OPEN_MAX)

struct openfile systemFileTable[SYSTEM_OPEN_MAX];

void openfileIncrRefCount(struct openfile *of) {
  if (of!=NULL)
    of->countRef++;
}

int sys_write(int fd, userptr_t buf, size_t size, int *retval) {
  struct iovec iov;
  struct uio ku;
  struct vnode *vn;
  struct openfile *of;
  int result, nwrite;
  void *kbuf;

  if (fd < 0 || fd > OPEN_MAX){
        *retval = EBADF;
        return -1;
    } 
    of = curproc->fileTable[fd];
    if (of == NULL){
        *retval = EBADF;
        return -1;
    }
    if(of->mode != O_WRONLY && of->mode!=O_RDWR){
        *retval = EBADF;
        return -1;
    }
    vn = of->vn;
    if (vn == NULL){
        *retval = EBADF;
        return -1;
    } 


    kbuf = kmalloc(size);
    if(kbuf == NULL){
        *retval = ENOMEM;
        return -1;
    }
    if(copyin(buf, kbuf, size)){
        *retval = EFAULT; //buf is outside the accessible address space
        kfree(kbuf);
        return -1;
    }
    lock_acquire(of->lock); //writing acquiring the lock to be the only one doing it
    uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_WRITE);

    result = VOP_WRITE(vn, &ku);
    if (result) {
        kfree(kbuf);
        *retval = result;
        return -1;
    }
    kfree(kbuf);
    of->offset = ku.uio_offset;
    nwrite = size - ku.uio_resid;
    lock_release(of->lock);
    return nwrite;
}

int sys_read(int fd, userptr_t buf, size_t size, int *retval) {
    struct iovec iov;
    struct uio ku;
    struct vnode *vn;
    struct openfile *of;
    int result, nread;
    void *kbuf;

    if (fd < 0 || fd >= OPEN_MAX) {
        *retval = EBADF;
        return -1;
    } 
    of = curproc->fileTable[fd];
    if (of == NULL) {
        *retval = EBADF;
        return -1;
    }
    if (of->mode != O_RDONLY && of->mode != O_RDWR) {
        *retval = EBADF;
        return -1;
    }
    vn = of->vn;
    if (vn == NULL) {
        *retval = EBADF;
        return -1;
    }

    kbuf = kmalloc(size);
    if (kbuf == NULL) {
        *retval = ENOMEM;
        return -1;
    }
    //Trying to copy the content of the user buffer in a Kernel buffer to make it check its validity
    if (copyin(buf, kbuf, size)) {
        kfree(kbuf);
        *retval = EFAULT;
        return -1;
    }

    lock_acquire(of->lock);
    uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_READ);
    result = VOP_READ(vn, &ku);

    if (result) {
        kfree(kbuf);
        *retval = result;
        return -1;
    }
    of->offset = ku.uio_offset;
    nread = size - ku.uio_resid;
    if (copyout(kbuf, buf, nread)) {
        *retval = EFAULT;
        return -1;
    }
    lock_release(of->lock);
    kfree(kbuf);
    return nread;
}

/*
 * file system calls for open/close
 */
int
sys_open(userptr_t path, int openflags, mode_t mode, int *retval)
{
  int fd, i;
  struct vnode *v;
  struct openfile *of=NULL;

  if (path == NULL || path == (userptr_t)0) {
    *retval = EFAULT;
    return -1;
  }

  /* Copying pathname to kernel side */
  char *kbuffer = (char *) kmalloc(PATH_MAX * sizeof(char));
  if (kbuffer == NULL) {
    *retval = ENOMEM;
    return -1;
  }

  size_t len;
  int err = copyinstr((const_userptr_t) path, kbuffer, PATH_MAX, &len); // may return EFAULT
  if (err) {
    kfree(kbuffer);
    *retval = EFAULT;
    return -1;
  }

  if((vaddr_t)path >= 0x80000000) {
    kfree(kbuffer);
    *retval = EFAULT;
    return -1;
  }

  err = vfs_open(kbuffer, openflags, mode, &v);
  kfree(kbuffer);
  if (err) {
    *retval = err;
    return -1;
  }

  for (i=0; i<SYSTEM_OPEN_MAX; i++) {
    if (systemFileTable[i].vn==NULL) {
      of = &systemFileTable[i];
      of->vn = v;
      of->offset = 0;
      break;
    }
  }

  if (of==NULL) { 
    // no free slot in system open file table
    vfs_close(v);
    return ENFILE;
  }

  // assigning openfile to current process filetable
  for (fd=STDERR_FILENO+1; fd<OPEN_MAX; fd++) {// skipping STDIN, STDOUT and STDERR
    if (curproc->fileTable[fd] == NULL) {
      curproc->fileTable[fd] = of;
      break;
    }
  }

  if(fd == OPEN_MAX) {
    // no free slot in process open file table
    vfs_close(v);
    return EMFILE;
  }

  // managing the way to read the file
  if (openflags & O_APPEND) {
    // retrieve file size
    struct stat filest;
    err = VOP_STAT(v, &filest);
    if (err) {
      curproc->fileTable[fd] = NULL;
      vfs_close(v);
      *retval = err;
      return -1;
    }
    // putting the offset at the end of the file (starting at the end)
    of->offset = filest.st_size;
  } else {
    // starting at the beginning, putting the offset of the file table at 0
    of->offset = 0;
  }

  // different modes
  switch(openflags & O_ACCMODE){
    case O_RDONLY: // read only mode
      of->mode = O_RDONLY;
      break;
    case O_WRONLY: // write only mode
			of->mode = O_WRONLY;
			break;
		case O_RDWR: // read and write mode
			of->mode = O_RDWR;
			break;
		default: // none of the specified mode
			vfs_close(v);
			curproc->fileTable[fd] = NULL;
      *retval = EINVAL;
			return -1;
  }

  // creating the lock on the file
  of->lock = lock_create("file_lock");
  // if the lock is equal to NULL means that something went wrong during the creation process
  if (of->lock == NULL) {
    vfs_close(v);
    curproc->fileTable[fd] = NULL;
    *retval = ENOMEM;
		return -1;
  }

  of->countRef = 1;
  
  // task completed, returning 0 and the fd
  *retval = 0;
  return fd;
}

/*
 * file system calls for open/close
 */
int
sys_close(int fd)
{
  
  // if fd is not a valid number or is not refering to a valid entry of the fileTable, return EBADF (Bad file number)
  if (fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
    return EBADF;       
  }

  struct openfile *of = curproc->fileTable[fd];
  // acquiring the lock to modify the value of count ref to the file, decreasing it by one
  lock_acquire(of->lock);
  of->countRef--;
  curproc->fileTable[fd] = NULL;

  if (of->countRef == 0) {
    vfs_close(of->vn);
    lock_release(of->lock);
    lock_destroy(of->lock);
  } else {
    lock_release(of->lock);
  }
  return 0;
}


#if OPT_SHELL
int sys_dup2(int old_fd, int new_fd, int *retval) {

    struct openfile *of;

    /* check if the curproc is valid*/
    KASSERT(curproc != NULL);

    /* validate input arguments */
    if (old_fd < 0 || old_fd >= OPEN_MAX || new_fd < 0 || new_fd >= OPEN_MAX) {
      /*fd must be in the valid range [0, OPEN_MAX]*/
        return EBADF;   // invalid file handler
    } else if (curproc->fileTable[old_fd] == NULL) {
      /*the old fd must refer to an open file*/
        return EBADF;   // invalid file handler
    } else if (old_fd == new_fd) {
        /* The two handles refer to the same "open" of the file - that is, they are references to the same object and share the same seek pointer. */
        *retval = old_fd; //return value
        //kprintf("\nretval = %d\n",*retval);
        return 0;   //no error 
    } 

    /* Ccheck if new_fd refers to an open file */
    if (curproc->fileTable[new_fd] != NULL) {
        
        /* Close the file currently associated with new_fd */
        of = curproc->fileTable[new_fd];
        lock_acquire(of->lock);
        curproc->fileTable[new_fd] = NULL;
        if (--of->countRef == 0) {

            /* If no processes are referencing this file, clean up resources */
            struct vnode *vn = of->vn;
            of->vn = NULL;
            vfs_close(vn);
        }
        lock_release(of->lock);
        of = NULL;
    }

    /* increment of the count references */
    of = curproc->fileTable[old_fd];
    lock_acquire(of->lock);
    of->countRef++;
    lock_release(of->lock);

    /* assignment  new_fd*/
    curproc->fileTable[new_fd] = of;   

    *retval = new_fd;
    return 0;
}
#endif

#if OPT_SHELL
int sys_chdir(const char *pathname) {

  // create a buffer
  char *kbuffer = (char *) kmalloc(PATH_MAX * sizeof(char));
  if (kbuffer == NULL) {
    return ENOMEM;
  }

  int result = copyinstr((const_userptr_t) pathname, kbuffer, PATH_MAX, NULL);
  if (result) {
    kfree(kbuffer);
    // Return an error if copyinstr encounter a problem it could be EFAULT
    return result;
  }

  // Set vnode to represent the directory
  struct vnode *vn = NULL;
  // Open dir pointed by pathname
  result = vfs_open(kbuffer, O_RDONLY, 0644, &vn);
  if (result) {
      kfree(kbuffer);
      return result; 
  }
  kfree(kbuffer);

  // Change current dir
  result = vfs_setcurdir(vn);
  if (result) {
      vfs_close(vn);
      return result;
  }

  // Close the vn because no longer needed. Dir already set
  vfs_close(vn);
  return 0; // no error
}
#endif

#if OPT_SHELL
int sys_getcwd(const char *buf, size_t buflen, int *retval) {

    /* check if the curproc is valid*/
    KASSERT(curthread != NULL);
    KASSERT(curthread->t_proc != NULL);

    //* Initialize a UIO structure for user-space buffer interaction */
    struct uio u;
    struct iovec iov;

    /* Set up the I/O vector with the user-provided buffer and its length */
    iov.iov_ubase = (userptr_t) buf;            
    iov.iov_len = buflen;

    /* Configure the UIO structure for reading into user space */
    u.uio_iov = &iov;
    u.uio_iovcnt = 1;
    u.uio_resid = buflen;
    u.uio_offset = 0;
    u.uio_segflg = UIO_USERSPACE;
    u.uio_rw = UIO_READ;
    u.uio_space = curthread->t_proc->p_addrspace;

    /* Fetch the current working directory path using VFS */
    int err = vfs_getcwd(&u);
    if (err) {
      return err;
    }

    /* Calculate the length of the returned path */
    *retval = buflen - u.uio_resid;
    
    return 0;
}
#endif
