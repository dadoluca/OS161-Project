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

/* max num of system wide open files */
#define SYSTEM_OPEN_MAX (10*OPEN_MAX)

#define USE_KERNEL_BUFFER 0

/* system open file table */
struct openfile {
  struct vnode *vn;
  off_t offset;
  int mode;
  unsigned int countRef;
  struct lock *lock;
};

struct openfile systemFileTable[SYSTEM_OPEN_MAX];

void openfileIncrRefCount(struct openfile *of) {
  if (of!=NULL)
    of->countRef++;
}

#if USE_KERNEL_BUFFER

static int
file_read(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio ku;
  int result, nread;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  kbuf = kmalloc(size);
  uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_READ);
  result = VOP_READ(vn, &ku);
  if (result) {
    return result;
  }
  of->offset = ku.uio_offset;
  nread = size - ku.uio_resid;
  copyout(kbuf,buf_ptr,nread);
  kfree(kbuf);
  return (nread);
}

static int
file_write(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio ku;
  int result, nwrite;
  struct vnode *vn;
  struct openfile *of;
  void *kbuf;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  kbuf = kmalloc(size);
  copyin(buf_ptr,kbuf,size);
  uio_kinit(&iov, &ku, kbuf, size, of->offset, UIO_WRITE);
  result = VOP_WRITE(vn, &ku);
  if (result) {
    return result;
  }
  kfree(kbuf);
  of->offset = ku.uio_offset;
  nwrite = size - ku.uio_resid;
  return (nwrite);
}

#else
static int
file_read(int fd, const void* buf_ptr, size_t size, int *retval) {
  struct iovec iov;
  struct uio u;
  int result;
  struct vnode *vn;
  struct openfile *of;

  if(buf_ptr == NULL){
    return EFAULT;
  }

  if(fd<0 || fd>=OPEN_MAX){
    return EBADF;
  } else if (curproc->fileTable[fd] == NULL){
    return EBADF;
  } else if(curproc->fileTable[fd]->mode == O_WRONLY) {
    return EBADF;
  }

  char *kbuffer = (char *) kmalloc((size+1)*sizeof(char));
  if(kbuffer == NULL) {
    return ENOMEM;
  }

  of = curproc->fileTable[fd];
  //if (of==NULL) return -1;
  vn = of->vn;
  //if (vn==NULL) return -1;

  lock_acquire(of->lock);

  iov.iov_kbase = kbuffer;
  iov.iov_len = size;
  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_resid = size;          // amount to read from the file
  u.uio_offset = of->offset;
  u.uio_segflg = UIO_SYSSPACE;
  u.uio_rw = UIO_READ;
  u.uio_space = NULL;

  result = VOP_READ(vn, &u);
  if (result) {
    kfree(kbuffer);
    lock_release(of->lock);
    return result;
  }

  of->offset = u.uio_offset;
  *retval = size - u.uio_resid;

  result = copyout(kbuffer, (userptr_t)buf_ptr, *retval);
  if(result) {
    kfree(kbuffer);
    lock_release(of->lock);
    return EFAULT;
  }

  lock_release(of->lock);
  kfree(kbuffer);
  return 0;
}

static int
file_write(int fd, userptr_t buf_ptr, size_t size) {
  struct iovec iov;
  struct uio u;
  int result, nwrite;
  struct vnode *vn;
  struct openfile *of;

  if (fd<0||fd>OPEN_MAX) return -1;
  of = curproc->fileTable[fd];
  if (of==NULL) return -1;
  vn = of->vn;
  if (vn==NULL) return -1;

  iov.iov_ubase = buf_ptr;
  iov.iov_len = size;

  u.uio_iov = &iov;
  u.uio_iovcnt = 1;
  u.uio_resid = size;          // amount to read from the file
  u.uio_offset = of->offset;
  u.uio_segflg =UIO_USERISPACE;
  u.uio_rw = UIO_WRITE;
  u.uio_space = curproc->p_addrspace;

  result = VOP_WRITE(vn, &u);
  if (result) {
    return result;
  }
  of->offset = u.uio_offset;
  nwrite = size - u.uio_resid;
  return (nwrite);
}

#endif

/*
 * file system calls for open/close
 */
int
sys_open(userptr_t path, int openflags, mode_t mode, int *retval)
{
  int fd, i;
  struct vnode *v;
  struct openfile *of=NULL;

  if (path == NULL) {
    return EFAULT;
  }

  /* Copying pathname to kernel side */
  char *kbuffer = (char *) kmalloc(PATH_MAX * sizeof(char));
    if (kbuffer == NULL) {
        return ENOMEM;
    }
    size_t len;
    int err = copyinstr((const_userptr_t) path, kbuffer, PATH_MAX, &len); // may return EFAULT
    if (err) {
        kfree(kbuffer);
        return EFAULT;
    }

  err = vfs_open((char *)path, openflags, mode, &v);
  if (err) {
    kfree(kbuffer);
    return err;
  }
  kfree(kbuffer);

  /* search system open file table */
  for (i=0; i<SYSTEM_OPEN_MAX; i++) {
    if (systemFileTable[i].vn==NULL) {
      of = &systemFileTable[i];
      of->vn = v;
      of->countRef = 1;
      break;
    }
  }

  if (of==NULL) { 
    // no free slot in system open file table
    return ENFILE;
  }
  else { // assigning openfile to current process filetable
    for (fd=STDERR_FILENO+1; fd<OPEN_MAX; fd++) {// skipping STDIN, STDOUT and STDERR
      if (curproc->fileTable[fd] == NULL) {
        curproc->fileTable[fd] = of;
        return fd;
      }
    }
    // no free slot in process open file table
    return EMFILE;
  }

  // managing the way to read the file
  if (openflags & O_APPEND) {
    // retrieve file size
    struct stat filest;
    err = VOP_STAT(curproc->fileTable[fd]->vn, &filest);
    if (err) {
      kfree(curproc->fileTable[fd]);
      curproc->fileTable[fd] = NULL;
      return err;
    }
    // putting the offset at the end of the file (starting at the end)
    curproc->fileTable[fd]->offset = filest.st_size;
  } else {
    // starting at the beginning, putting the offset of the file table at 0
    curproc->fileTable[fd]->offset = 0;
  }

  // different modes
  switch(openflags & O_ACCMODE){
    case O_RDONLY: // read only mode
      curproc->fileTable[fd]->mode = O_RDONLY;
      break;
    case O_WRONLY: // write only mode
			curproc->fileTable[fd]->mode = O_WRONLY;
			break;
		case O_RDWR: // read and write mode
			curproc->fileTable[fd]->mode = O_RDWR;
			break;
		default: // none of the specified mode
			vfs_close(curproc->fileTable[fd]->vn);
			kfree(curproc->fileTable[fd]);
			curproc->fileTable[fd] = NULL;
			return EINVAL;
  }

  // creating the lock on the file
  curproc->fileTable[fd]->lock = lock_create("FILE_LOCK");
  // if the lock is equal to NULL means that something went wrong during the creation process
  if (curproc->fileTable[fd]->lock == NULL) {
    vfs_close(curproc->fileTable[fd]->vn);
    kfree(curproc->fileTable[fd]);
    curproc->fileTable[fd] = NULL;
    return ENOMEM;
  }
  
  // task completed, returning 0 and the fd
  *retval = fd;
  return 0;
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
  curproc->fileTable[fd] = NULL;
  of->countRef -= 1;
  if (of->countRef <= 0) {
    struct vnode *vn = of->vn;
    of->vn = NULL;
    vfs_close(vn);
  }
  lock_release(of->lock);
  return 0;
}

/*
 * simple file system calls for write/read
 */
int
sys_write(int fd, userptr_t buf_ptr, size_t size)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd!=STDOUT_FILENO && fd!=STDERR_FILENO) {
    return file_write(fd, buf_ptr, size);
  }

  for (i=0; i<(int)size; i++) {
    putch(p[i]);
  }

  return (int)size;
}

int
sys_read(int fd, const void* buf_ptr, size_t size, int *retval)
{
  int i;
  char *p = (char *)buf_ptr;

  if (fd!=STDIN_FILENO) {
    return file_read(fd, buf_ptr, size, retval);
  }

  for (i=0; i<(int)size; i++) {
    p[i] = getch();
    if (p[i] < 0) 
      return i;
  }

  *retval = (int)size;
  return 0;
}