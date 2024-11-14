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
