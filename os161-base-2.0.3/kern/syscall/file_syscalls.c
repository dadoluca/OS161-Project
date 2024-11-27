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

#if OPT_SHELL
int sys_write(int fd, userptr_t buf, size_t size, int *retval) {
  struct iovec iov;
  struct uio ku;
  struct vnode *vn;
  struct openfile *of;
  int result, nwrite;
  void *kbuf;

  /* checking if fd is valid */
  if (fd < 0 || fd > OPEN_MAX){
    return EBADF;
  } 
  of = curproc->fileTable[fd];
  /* checking if the file is on the fileTable*/
  if (of == NULL){
    return EBADF;
  }
  /* checking if the file is one in a correct mode */
  if(of->mode != O_WRONLY && of->mode!=O_RDWR){
    return EBADF;
  }
  vn = of->vn;
  /* checking if the vnode, associated with the file, exists */
  if (vn == NULL){
    return EBADF;
  }

  /* allocate a temporary kernel buffer for the operation */
  kbuf = kmalloc(size);
  if(kbuf == NULL){
    return ENOMEM;
  }

  /* copying the content of the user buffer into the kernel buffer */
  if(copyin(buf, kbuf, size)){
      kfree(kbuf);
      return EFAULT; //buf is outside the accessible address space
  }

  /* acquiring the lock */
  lock_acquire(of->lock);

  /* initializing the uio structure to the read operation */
  uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_WRITE);

  /* performing the write operation */
  result = VOP_WRITE(vn, &ku);
  if (result) {
      kfree(kbuf);
      return result;
  }
  /* freeing the kernel buffer */
  kfree(kbuf);

  /* updating the file offset based on the number of bytes written */
  of->offset = ku.uio_offset;
  /* computing the actual written bytes */
  nwrite = size - ku.uio_resid;
  /* release the lock */
  lock_release(of->lock);

  *retval = nwrite;

  return 0;
}
#endif

#if OPT_SHELL
int sys_read(int fd, userptr_t buf, size_t size, int *retval) {
    struct iovec iov;
    struct uio ku;
    struct vnode *vn;
    struct openfile *of;
    int result, nread;
    void *kbuf;

    /* checking if fd is valid */
    if (fd < 0 || fd >= OPEN_MAX) {
      return EBADF;
    } 
    of = curproc->fileTable[fd];
    /* checking if the file is on the fileTable*/
    if (of == NULL) {
      return EBADF;
    }
    /* checking if the file is one in a correct mode */
    if (of->mode != O_RDONLY && of->mode != O_RDWR) {
      return EBADF;
    }
    vn = of->vn;
    /* checking if the vnode, associated with the file, exists */
    if (vn == NULL) {
      return EBADF;
    }

    /* allocate a temporary kernel buffer for the operation */
    kbuf = kmalloc(size);
    if (kbuf == NULL) {
      return ENOMEM;
    }

    /* copying the content of the user buffer into the kernel buffer */
    if (copyin(buf, kbuf, size)) {
      kfree(kbuf);
      return EFAULT;
    }

    /* acquiring the lock */
    lock_acquire(of->lock);

    /* initializing the uio structure to the read operation */
    uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_READ);

    /* performing the read operation */
    result = VOP_READ(vn, &ku);
    if (result) {
      kfree(kbuf);
      return result;
    }

    /* updating the file offset based on the number of bytes read */
    of->offset = ku.uio_offset;
    /* computing the actual read bytes */
    nread = size - ku.uio_resid;
    
    /* copying the read data from the kernel buffer to the user buffer */
    if (copyout(kbuf, buf, nread)) {
      return EFAULT;
    }

    /* release the lock */
    lock_release(of->lock);

    /* freeing the kernel buffer */
    kfree(kbuf);
    
    *retval = nread;

    return 0;
}
#endif

/*
 * file system calls for open/close
 */
#if OPT_SHELL
int
sys_open(userptr_t path, int openflags, mode_t mode, int *retval)
{
  int fd, i;
  struct vnode *v;
  struct openfile *of=NULL;
  size_t len;

  /* checking if path is valid */
  if (path == NULL || path == (userptr_t)0) {
    return EFAULT;
  }

  /* allocating a kernel buffer for copying the file path */
  char *kbuffer = (char *) kmalloc(PATH_MAX * sizeof(char));
  if (kbuffer == NULL) {
    return ENOMEM;
  }

  /* copying the file path from the user to the kernel buffer */
  int err = copyinstr((const_userptr_t) path, kbuffer, PATH_MAX, &len); // may return EFAULT
  if (err) {
    kfree(kbuffer);
    return EFAULT;
  }

  /* making sure that the path is not in the kernel address space */
  if((vaddr_t)path >= 0x80000000) {
    kfree(kbuffer);
    return EFAULT;
  }

  /* opening the vnode associated with the path */
  err = vfs_open(kbuffer, openflags, mode, &v);
  kfree(kbuffer);
  if (err) {
    return err;
  }

  /* finding an available slot in the system file table, setting parameters */
  for (i=0; i<SYSTEM_OPEN_MAX; i++) {
    if (systemFileTable[i].vn==NULL) {
      of = &systemFileTable[i];
      of->vn = v;
      of->offset = 0;
      break;
    }
  }

  /* no free slot in system file table */
  if (of==NULL) { 
    vfs_close(v);
    return ENFILE;
  }

  /* finding an available slot in the current process file table */
  for (fd=STDERR_FILENO+1; fd<OPEN_MAX; fd++) { // skipping STDIN, STDOUT and STDERR
    if (curproc->fileTable[fd] == NULL) {
      curproc->fileTable[fd] = of;
      break;
    }
  }

  /* no free slot in process open file table */
  if(fd == OPEN_MAX) {
    vfs_close(v);
    return EMFILE;
  }

  /* managing the way to read the file */
  if (openflags & O_APPEND) {
    struct stat filest;
    /* retrieve file infos */
    err = VOP_STAT(v, &filest);

    if (err) {
      curproc->fileTable[fd] = NULL;
      vfs_close(v);
      return err;
    }
    /* putting the offset at the end of the file (starting at the end) */
    of->offset = filest.st_size;
  } else {

    /* starting at the beginning, putting the offset of the file table at 0 */
    of->offset = 0;
  }

  /* setting the file's access mode */
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
      return EINVAL;
  }

  /* creating the lock on the file */
  of->lock = lock_create("file_lock");

  /* if the lock is equal to NULL means that something went wrong during the creation process */
  if (of->lock == NULL) {
    vfs_close(v);
    curproc->fileTable[fd] = NULL;
    return ENOMEM;
  }

  /* update the countRef, meaning that we have opened the file */
  of->countRef = 1;
  
  *retval = fd;

  return 0;
}
#endif

/*
 * file system calls for open/close
 */
#if OPT_SHELL
int
sys_close(int fd)
{
  /* checking if fd is valid and refers to a valid entry of the file table */
  if (fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
    return EBADF;       
  }

  struct openfile *of = curproc->fileTable[fd];

  /* acquire the lock */
  lock_acquire(of->lock);

  /* decrease the countRef, meaning that we have closed a file */
  of->countRef--;
  /* removing, from the file table, the refers to the fd */
  curproc->fileTable[fd] = NULL;

  /* if there are no more references to the file exists, clean up resources */
  if (of->countRef == 0) {
    /* close the vnode */
    vfs_close(of->vn);
    /* release the lock */
    lock_release(of->lock);
    /* destroy the lock */
    lock_destroy(of->lock);
  } else {
    /* release the lock */
    lock_release(of->lock);
  }
  return 0;
}
#endif

#if OPT_SHELL
int sys_dup2(int old_fd, int new_fd, int *retval) {
  struct openfile *of;

  /* checking if the curproc is valid */
  KASSERT(curproc != NULL);

  /* validate input arguments */
  if (old_fd < 0 || old_fd >= OPEN_MAX || new_fd < 0 || new_fd >= OPEN_MAX) {
    /* fd must be in the valid range [0, OPEN_MAX] */
    return EBADF;   // invalid file handler
  } else if (curproc->fileTable[old_fd] == NULL) {
    /* the old fd must refer to an open file */
    return EBADF;   // invalid file handler
  } else if (old_fd == new_fd) {
      /* the two handles refer to the same "open" of the file - that is, they are references to the same object and share the same seek pointer */
    *retval = old_fd; //return value
    return 0; 
  } 
  /* check if new_fd refers to an open file */
  if (curproc->fileTable[new_fd] != NULL) {
    /* close the file currently associated with new_fd */
    of = curproc->fileTable[new_fd];

    /* acquire the lock */
    lock_acquire(of->lock);

    curproc->fileTable[new_fd] = NULL;

    /* if no processes are referencing this file, clean up resources */
    if (--of->countRef == 0) {
      struct vnode *vn = of->vn;
      of->vn = NULL;
      /* close the vnode */
      vfs_close(vn);
    }
    /* release the lock */
    lock_release(of->lock);
    of = NULL;
  }

  /* increment of the count references */
  of = curproc->fileTable[old_fd];
  /* acquire the lock */
  lock_acquire(of->lock);
  of->countRef++;
  /* release the lock */
  lock_release(of->lock);

  /* assignment  new_fd */
  curproc->fileTable[new_fd] = of;

  *retval = new_fd;

  return 0;
}
#endif

#if OPT_SHELL
int sys_lseek(int fd, off_t pos, int whence, int64_t* retval) {

  /* checking if the curproc is valid */
  KASSERT(curproc != NULL);

  /* checking if fd is valid */
  if(fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
    return EBADF;
  }
  /* checking if the openfile associated to the fd is valid */
  struct openfile *of = curproc->fileTable[fd];
  if(of == NULL) {
    return EBADF;
  }

  /* checking if the object is a seekable one */
  if(!VOP_ISSEEKABLE(of->vn)) {
    return ESPIPE;
  }


  struct stat info;
  int err;
  int new_off;

  /* take the lock */
  lock_acquire(of->lock);

  /* switch for each different whence */
  switch (whence) {
    /* setting position as pos */
    case SEEK_SET:
      if (pos < 0) {
        lock_release(of->lock);
        return EINVAL;
      }
      new_off = pos;
      break;
    /* setting position as position + pos */
    case SEEK_CUR:
      if (pos < 0 && -pos > of->offset) {
        /* release the lock, bc there is an error */
        lock_release(of->lock);
        return EINVAL;
      }
      new_off = of->offset + pos;
      break;
    /* setting position as end-of-file + pos */
    case SEEK_END:
      err = VOP_STAT(of->vn, &info);
      if (err) {
        /* release the lock, bc there is an error */
        lock_release(of->lock);
        return err;
      }
      /* checking if -pos higher then file size */
      if (pos < 0 && -pos > info.st_size) { 
        lock_release(of->lock);
        return EINVAL;
      }
      new_off = info.st_size + pos; //TODO: check the sign
      break;
    default:
      lock_release(of->lock);
      return EINVAL;
  }

  /* updating the offset */
  of->offset = new_off;

  /* syscall finish, release the lock */
  lock_release(of->lock);

  *retval = new_off;
  /* success */
  return 0;
}
#endif

#if OPT_SHELL
int sys_chdir(const char *pathname) {

  /* create a buffer */
  char *kbuffer = (char *) kmalloc(PATH_MAX * sizeof(char));
  if (kbuffer == NULL) {
    return ENOMEM;
  }

  /* create a space */
  int result = copyinstr((const_userptr_t) pathname, kbuffer, PATH_MAX, NULL);
  if (result) {
    kfree(kbuffer);
    /* Return an error if copyinstr encounter a problem it could be EFAULT */
    return result;
  }

  /* Set vnode to represent the directory */
  struct vnode *vn = NULL;
  /* Open dir pointed by pathname */
  result = vfs_open(kbuffer, O_RDONLY, 0644, &vn);
  if (result) {
      kfree(kbuffer);
      return result;  /* return the error from the function vfs_open*/
  }
  kfree(kbuffer);

  /* Change current dir */
  result = vfs_setcurdir(vn);
  if (result) {
      vfs_close(vn);
      return result;
  }

  /* Close the vn because no longer needed. Dir already set */
  vfs_close(vn);
  /* success */
  return 0;
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
