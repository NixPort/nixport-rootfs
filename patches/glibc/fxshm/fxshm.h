/* fexdroid: internal interface of the userspace SysV IPC implementation:
   shared memory (fxshm-client.c, used by shmget.c, shmat.c, shmdt.c and
   shmctl.c) and semaphores (fxsem-client.c, used by semget.c, semtimedop.c,
   semctl.c), plus the syscall() wrapper (aarch64/fxd-syscall.c).  */
#ifndef _FXSHM_H
#define _FXSHM_H 1

#include <sys/shm.h>
#include <sys/sem.h>
#include <time.h>

extern int __fxshm_get (key_t key, size_t size, int shmflg) attribute_hidden;
extern void *__fxshm_at (int shmid, const void *shmaddr, int shmflg)
  attribute_hidden;
extern int __fxshm_dt (const void *shmaddr) attribute_hidden;
extern int __fxshm_ctl (int shmid, int cmd, struct shmid_ds *buf)
  attribute_hidden;

extern int __fxsem_get (key_t key, int nsems, int semflg) attribute_hidden;
extern int __fxsem_timedop (int semid, struct sembuf *sops, size_t nsops,
                            const struct timespec *timeout) attribute_hidden;
/* ARG is the raw union semun value (as passed to the semctl syscall).  */
extern int __fxsem_ctl (int semid, int semnum, int cmd, unsigned long int arg)
  attribute_hidden;

struct fxshm_req;
struct fxshm_resp;
extern int __fxshm_call (struct fxshm_req *q, struct fxshm_resp *r,
                         int *fd_out) attribute_hidden;
extern unsigned int __fxshm_conn_gen attribute_hidden;

#endif /* fxshm.h */
