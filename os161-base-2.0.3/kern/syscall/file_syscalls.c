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

  /* search system open file table */
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

int sys_chdir(const char *path) {
  struct vnode *vn;
  char *kbuffer;
  
  KASSERT(curthread != NULL);
  KASSERT(curthread->t_proc != NULL);

  kbuffer = (char *)kmalloc(PATH_MAX);
  if (kbuffer == NULL) {
    return ENOMEM;
  }

  int err = copyinstr((const_userptr_t)path, kbuffer, PATH_MAX, NULL);
  if (err) {
    kfree(kbuffer);
    return err;
  }

  err = vfs_open(kbuffer, O_RDONLY, 0, &vn);
  kfree(kbuffer);
  if (err) {
    return err;
  }

  err = vfs_setcurdir(vn);
  if (err) {
    vfs_close(vn);
    return err;
  }

  vfs_close(vn);
  return 0;
}

int sys_lseek(int fd, off_t pos, int whence, int64_t* retval) {

  KASSERT(curproc != NULL);

  if(fd < 0 || fd >= OPEN_MAX || curproc->fileTable[fd] == NULL) {
    return EBADF;
  }

  struct openfile *of = curproc->fileTable[fd];
  if(of == NULL) {
    return EBADF;
  }

  // checking if the object is a seekable one
  if(!VOP_ISSEEKABLE(of->vn)) {
    return ESPIPE;
  }

  struct stat info;
  int err;
  int new_off;

  lock_acquire(of->lock);

  switch (whence) {
    // setting position as pos
    case SEEK_SET:
      if (pos < 0) {
        lock_release(of->lock);
        return EINVAL;
      }
      new_off = pos;
      break;
    // setting position as position + pos
    case SEEK_CUR:
      if (pos < 0 && -pos > of->offset) {
        lock_release(of->lock);
        return EINVAL;
      }
      new_off = of->offset + pos;
      break;
    // setting position as end-of-file + pos
    case SEEK_END:
      err = VOP_STAT(of->vn, &info);
      if (err) {
        lock_release(of->lock);
        return err;
      }
      if (pos < 0 && -pos > info.st_size) { // checking if -pos higher then file size
        lock_release(of->lock);
        return EINVAL;
      }
      new_off = info.st_size + pos;
      break;
    default:
      lock_release(of->lock);
      return EINVAL;
  }

  // updating the offset
  of->offset = new_off;
  lock_release(of->lock);

  *retval = new_off;
  return 0;
}

/*
* System call interface function for making two file descriptors point to the
* same file handle
*/
int sys_dup2(int oldfd, int newfd, int*retval) {

    struct openfile *of;

    /* SOME ASSERTION */
    KASSERT(curproc != NULL);

    /* CHECKING INPUT ARGUMENTS */
    if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX) {
        return EBADF;   // invalid file handler
    } else if (curproc->fileTable[oldfd] == NULL) {
        return EBADF;   // invalid file handler
    } else if (oldfd == newfd) {
        *retval = newfd;
        return 0;           // using dup2 to clone a file handle onto itself has no effect
    } 

    /* CHECKING WHETHER newfd REFERS TO AN OPEN FILE */
    if (curproc->fileTable[newfd] != NULL) {
        
        /* newfd REFERS TO AN OPEN FILE --> CLOSE IT */
        of = curproc->fileTable[newfd];
        lock_acquire(of->lock);
        curproc->fileTable[newfd] = NULL;
        if (--of->countRef == 0) {

            /* NO MORE PROCESS REFER TO THIS FILE, CLOSING ALSO VNODE */
            struct vnode *vn = of->vn;
            of->vn = NULL;
            vfs_close(vn);
        }
        lock_release(of->lock);
        of = NULL;
    }

    /* INCREMENTIG COUNTING REFERENCES */
    of = curproc->fileTable[oldfd];
    lock_acquire(of->lock);
    of->countRef++;
    lock_release(of->lock);

    /* ASSIGNING TO NEW FILE DESCRIPTOR */
    curproc->fileTable[newfd] = of;     // i.e.     curproc->fileTable[newfd] = curproc->fileTable[oldfd];

    /* TASK COMPLETED SUCCESSFULLY */
    *retval = newfd;
    return 0;
}

int sys_getcwd(char *buf, size_t buflen, int *retval) {
  KASSERT(curthread != NULL);
  KASSERT(curthread->t_proc != NULL);

  if (buf == NULL) {
    return EFAULT;
  }
  if (buflen == 0) {
    return EINVAL;
  }

  struct uio u;
  struct iovec iov;

  iov.iov_ubase = (userptr_t)buf;
  iov.iov_len = buflen;

  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_resid = buflen;
  u.uio_offset = 0;
  u.uio_segflg = UIO_USERSPACE;
  u.uio_rw = UIO_READ;
  u.uio_space = curthread->t_proc->p_addrspace;

  int err = vfs_getcwd(&u);
  if (err) {
      return err;
  }
  if (u.uio_resid > 0) {
      return ERANGE;
  }

  *retval = buflen - u.uio_resid;
  return 0;
}