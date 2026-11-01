/* fexdroid: wire protocol between the patched glibc (SysV shm client) and
   fxshmd, the per-app shared-memory registry daemon.

   Android's app seccomp filter kills processes that issue the SysV IPC
   syscalls, so glibc's shmget/shmat/shmdt/shmctl are re-implemented on top of
   memfd_create + a tiny daemon that owns one memfd per segment and hands out
   duplicates over a unix SOCK_SEQPACKET socket (SCM_RIGHTS).  Semaphore sets
   (semget/semop/semtimedop/semctl) are memfds from the same daemon.  This header is
   the single source of truth: tools/fxshmd/fxshmd.c includes it directly and
   scripts/lib/make-glibc-sysvipc-patch.py copies it into the glibc patch.

   All fields are fixed-width; client and daemon are always built for the same
   ABI (arm64 rootfs), so no byte-order handling.  */
#ifndef FXSHM_PROTO_H
#define FXSHM_PROTO_H 1

#include <stdint.h>

#define FXSHM_PROTO_VERSION 1u

/* Socket and lock file live in this directory under the rootfs /tmp.  */
#define FXSHM_DIR_NAME  ".fxshm"
#define FXSHM_SOCK_NAME "sock"
#define FXSHM_LOCK_NAME "lock"

/* Environment overrides (secure_getenv on the client side).  */
#define FXSHM_ENV_SOCKET "FXD_SHM_SOCKET"   /* full path of the socket */
#define FXSHM_ENV_DAEMON "FXD_SHMD"         /* executable to spawn instead of ld.so+fxshmd */

enum fxshm_op
{
  FXSHM_OP_PING = 0,
  FXSHM_OP_GET = 1,      /* shmget: key, size, flags, pid          -> result = id */
  FXSHM_OP_ATTACH = 2,   /* shmat:  id, flags, pid                 -> fd via SCM_RIGHTS, seg */
  FXSHM_OP_DETACH = 3,   /* shmdt:  id, pid                        -> 0 */
  FXSHM_OP_INHERIT = 4,  /* after fork: id, count(size)            -> 0 (no fd) */
  FXSHM_OP_STAT = 5,     /* shmctl IPC_STAT: id                    -> seg */
  FXSHM_OP_SET = 6,      /* shmctl IPC_SET:  id, uid, gid, mode    -> 0 */
  FXSHM_OP_RMID = 7,     /* shmctl IPC_RMID: id                    -> 0 */
  /* SysV semaphores (layout of a set: fxsem-layout.h).  Sets have their own
     key and id namespace, as in the kernel.  */
  FXSHM_OP_SEM_GET = 8,  /* semget: key, size = nsems, flags, pid  -> result = id,
                            fd via SCM_RIGHTS, seg.segsz = bytes to map */
  FXSHM_OP_SEM_OPEN = 9, /* id                                    -> fd, seg.segsz */
  FXSHM_OP_SEM_RMID = 10,/* semctl IPC_RMID: id                   -> 0 (sleepers get EIDRM) */
  FXSHM_OP_SEM_UNDO = 11,/* pid: apply pid's SEM_UNDO adjustments when this
                            connection hangs up (process exit/exec)  -> 0 */
};

struct fxshm_req
{
  uint32_t version;   /* FXSHM_PROTO_VERSION */
  uint32_t op;        /* enum fxshm_op */
  int32_t id;         /* shmid for ATTACH/DETACH/INHERIT/STAT/SET/RMID */
  int32_t key;        /* key_t for GET */
  uint64_t size;      /* GET: requested size; INHERIT: attachment count */
  int32_t flags;      /* GET: shmflg; ATTACH: shmflg */
  int32_t pid;        /* caller pid, for shm_cpid/shm_lpid */
  uint32_t uid, gid, mode;   /* SET */
};

/* Mirrors the fields of struct shmid_ds.  */
struct fxshm_seg
{
  int32_t key;
  uint32_t uid, gid, cuid, cgid, mode;
  uint64_t segsz;
  int64_t atime, dtime, ctime;
  int32_t cpid, lpid;
  uint64_t nattch;
};

struct fxshm_resp
{
  int32_t result;     /* >= 0 on success, -1 on error */
  int32_t err;        /* errno value when result < 0 */
  struct fxshm_seg seg;
};

#endif /* fxshm-proto.h */
