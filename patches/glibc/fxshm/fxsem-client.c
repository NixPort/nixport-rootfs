/* fexdroid: SysV semaphores without the sem* syscalls.
   Copyright (C) 2026 fexdroid contributors.  MIT license (fexdroid); this file
   is added to glibc under the LGPL-2.1-or-later when built into libc.

   Android's app seccomp filter kills a process with SIGSYS when it issues
   semget/semop/semtimedop/semctl.  Every semaphore set here is a memfd
   created by fxshmd (tools/fxshmd/fxshmd.c) and mapped MAP_SHARED by each
   process that uses it; the layout is in fxsem-layout.h.

     semget      -> SEM_GET   the daemon creates/looks up the set and passes
                              its memfd, which we map and cache by id
     semop,
     semtimedop  -> no daemon round trip: all operations are applied
                    atomically under the set's process-shared lock; sleepers
                    wait with FUTEX_WAIT_BITSET on the set's sequence word
     semctl      -> IPC_RMID goes to the daemon (SEM_RMID, sleepers get
                    EIDRM); everything else is done in the mapping

   SEM_UNDO: adjustments are kept per process (pid) in the mapping, updated
   atomically with the operation.  The first SEM_UNDO operation of a process
   registers its pid on its daemon connection (SEM_UNDO op); when that
   connection hangs up (exit, crash, or exec without re-registering) fxshmd
   applies the adjustments like the kernel's exit_sem.

   Only futex, kill(pid, 0), getpid, clock_gettime, mmap/munmap and the
   daemon socket are used.  None of it is a cancellation point.  */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <time.h>
#include <unistd.h>
#include <libc-lock.h>
#include <register-atfork.h>
#include <not-cancel.h>
#include <sysdep.h>
#include <fxshm-proto.h>
#include <fxsem-layout.h>
#include <fxshm.h>

#ifndef FUTEX_WAIT_BITSET
# define FUTEX_WAIT_BITSET 9
#endif
#ifndef FUTEX_BITSET_MATCH_ANY
# define FUTEX_BITSET_MATCH_ANY 0xffffffff
#endif

/* semctl's IPC_64 flag (0x100); callers such as FEX's i386 ipc() emulation
   may leave it in CMD.  */
#define FXSEM_IPC_64 0x100

static long
fxsem_sys_futex (uint32_t *uaddr, int op, uint32_t val,
                 const struct timespec *rel_timeout)
{
  return INTERNAL_SYSCALL_CALL (futex, uaddr, op, val, rel_timeout, NULL, 0);
}

static int
fxsem_sys_alive (int32_t pid)
{
  long r = INTERNAL_SYSCALL_CALL (kill, pid, 0);
  return !(INTERNAL_SYSCALL_ERROR_P (r) && INTERNAL_SYSCALL_ERRNO (r) == ESRCH);
}

static int64_t
now_realtime (void)
{
  struct timespec ts;
  __clock_gettime (CLOCK_REALTIME, &ts);
  return ts.tv_sec;
}

/* ---- per-process cache of mapped sets ------------------------------------ */

struct semmap
{
  int id;
  unsigned int gen;       /* __fxshm_conn_gen when mapped */
  struct fxsem_hdr *h;
  size_t size;
  unsigned int users;     /* callers currently using H (under cache_lock) */
  bool dead;              /* unlinked; unmapped when USERS drops to 0 */
  struct semmap *next;
};

__libc_lock_define_initialized (static, cache_lock);
__libc_once_define (static, once);
static struct semmap *cache;
static pid_t undo_pid;          /* pid registered for SEM_UNDO ... */
static unsigned int undo_gen;   /* ... on this daemon connection generation */

static void
fork_prepare (void)
{
  __libc_lock_lock (cache_lock);
}

static void
fork_parent (void)
{
  __libc_lock_unlock (cache_lock);
}

static void
fork_child (void)
{
  /* The mappings are inherited (MAP_SHARED); threads that were using them
     do not exist here, so their USERS references just keep the mappings.  */
  __libc_lock_init (cache_lock);
}

static void
init_once (void)
{
  __register_atfork (fork_prepare, fork_parent, fork_child, NULL);
}

static struct semmap *
cache_find_locked (int id, unsigned int gen)
{
  for (struct semmap *m = cache; m != NULL; m = m->next)
    if (m->id == id && m->gen == gen)
      return m;
  return NULL;
}

static struct semmap *
cache_get (int id)
{
  unsigned int gen = __atomic_load_n (&__fxshm_conn_gen, __ATOMIC_ACQUIRE);
  __libc_lock_lock (cache_lock);
  struct semmap *m = cache_find_locked (id, gen);
  if (m != NULL)
    m->users++;
  __libc_lock_unlock (cache_lock);
  return m;
}

static void
cache_put (struct semmap *m)
{
  __libc_lock_lock (cache_lock);
  bool release = --m->users == 0 && m->dead;
  __libc_lock_unlock (cache_lock);
  if (release)
    {
      __munmap (m->h, m->size);
      free (m);
    }
}

/* The set was removed: forget it (the mapping goes with the last user).  */
static void
cache_kill (struct semmap *m)
{
  __libc_lock_lock (cache_lock);
  if (!m->dead)
    {
      struct semmap **pp = &cache;
      while (*pp != NULL && *pp != m)
        pp = &(*pp)->next;
      if (*pp == m)
        *pp = m->next;
      m->dead = true;
    }
  __libc_lock_unlock (cache_lock);
}

/* Maps the set ID from FD (SIZE bytes) and caches it.  Returns it with a
   reference, or NULL with errno set.  */
static struct semmap *
cache_insert_fd (int id, int fd, uint64_t size)
{
  unsigned int gen = __atomic_load_n (&__fxshm_conn_gen, __ATOMIC_ACQUIRE);
  struct semmap *m = cache_get (id);
  if (m != NULL)
    return m;
  if (size < sizeof (struct fxsem_hdr) || size > ((uint64_t) 1 << 40))
    {
      __set_errno (EINVAL);
      return NULL;
    }
  void *p = __mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    return NULL;
  struct fxsem_hdr *h = p;
  if (__atomic_load_n (&h->magic, __ATOMIC_ACQUIRE) != FXSEM_MAGIC
      || h->id != id || h->nsems <= 0
      || fxsem_size (h->nsems) > size)
    {
      __munmap (p, size);
      __set_errno (EINVAL);
      return NULL;
    }
  struct semmap *n = malloc (sizeof *n);
  if (n == NULL)
    {
      __munmap (p, size);
      return NULL;
    }
  n->id = id;
  n->gen = gen;
  n->h = h;
  n->size = size;
  n->users = 1;
  n->dead = false;
  __libc_lock_lock (cache_lock);
  m = cache_find_locked (id, gen);
  if (m != NULL)
    m->users++;              /* another thread won the race */
  else
    {
      n->next = cache;
      cache = n;
    }
  __libc_lock_unlock (cache_lock);
  if (m != NULL)
    {
      __munmap (p, size);
      free (n);
      return m;
    }
  return n;
}

/* The mapping of set SEMID, with a reference (cache_put), or NULL/errno.  */
static struct semmap *
acquire (int semid)
{
  __libc_once (once, init_once);
  if (semid < 0)
    {
      __set_errno (EINVAL);
      return NULL;
    }
  struct semmap *m = cache_get (semid);
  if (m != NULL)
    return m;
  struct fxshm_req q = { .op = FXSHM_OP_SEM_OPEN, .id = semid };
  struct fxshm_resp r;
  int fd;
  if (__fxshm_call (&q, &r, &fd) < 0)
    return NULL;
  if (fd < 0)
    {
      __set_errno (EINVAL);
      return NULL;
    }
  m = cache_insert_fd (semid, fd, r.seg.segsz);
  int saved = errno;
  __close_nocancel (fd);
  __set_errno (saved);
  return m;
}

/* Owner-only permission model: the app sandbox has a single uid.  */
static bool
permitted (const struct fxsem_hdr *h, int want)
{
  uint32_t mode = __atomic_load_n (&h->mode, __ATOMIC_RELAXED);
  return (mode & want) == (uint32_t) want;
}

/* ---- semget ---------------------------------------------------------------- */

int
__fxsem_get (key_t key, int nsems, int semflg)
{
  __libc_once (once, init_once);
  if (nsems < 0 || nsems > FXSEM_SEMMSL)
    {
      __set_errno (EINVAL);
      return -1;
    }
  struct fxshm_req q = { .op = FXSHM_OP_SEM_GET, .key = key,
                         .size = (uint64_t) nsems, .flags = semflg };
  struct fxshm_resp r;
  int fd;
  if (__fxshm_call (&q, &r, &fd) < 0)
    return -1;
  int id = r.result;
  if (fd >= 0)
    {
      /* Map it now; the first semop is then free of daemon round trips.
         Failure is not fatal here, acquire() retries.  */
      int saved = errno;
      struct semmap *m = cache_insert_fd (id, fd, r.seg.segsz);
      if (m != NULL)
        cache_put (m);
      __close_nocancel (fd);
      __set_errno (saved);
    }
  return id;
}

/* ---- semop / semtimedop ------------------------------------------------------ */

/* Undo slot of PID in H, allocating one if needed; -1 if none is free.  */
static int
undo_slot (struct fxsem_hdr *h, int32_t pid)
{
  int free_slot = -1;
  for (int i = 0; i < FXSEM_UNDO_SLOTS; i++)
    {
      if (h->undo_pid[i] == pid)
        return i;
      if (h->undo_pid[i] == 0 && free_slot < 0)
        free_slot = i;
    }
  if (free_slot < 0)
    {
      /* Recycle a slot with nothing to undo.  */
      for (int i = 0; i < FXSEM_UNDO_SLOTS && free_slot < 0; i++)
        {
          int32_t *adj = fxsem_adj (h, i);
          bool zero = true;
          for (int32_t k = 0; k < h->nsems && zero; k++)
            zero = adj[k] == 0;
          if (zero)
            free_slot = i;
        }
      if (free_slot < 0)
        return -1;
    }
  h->undo_pid[free_slot] = pid;
  memset (fxsem_adj (h, free_slot), 0, sizeof (int32_t) * h->nsems);
  return free_slot;
}

/* Tell fxshmd to apply our SEM_UNDO adjustments when we go away.  */
static void
register_undo (pid_t pid)
{
  unsigned int gen = __atomic_load_n (&__fxshm_conn_gen, __ATOMIC_ACQUIRE);
  if (__atomic_load_n (&undo_pid, __ATOMIC_RELAXED) == pid
      && __atomic_load_n (&undo_gen, __ATOMIC_RELAXED) == gen)
    return;
  int saved = errno;
  struct fxshm_req q = { .op = FXSHM_OP_SEM_UNDO };
  struct fxshm_resp r;
  if (__fxshm_call (&q, &r, NULL) == 0)
    {
      __atomic_store_n (&undo_gen, gen, __ATOMIC_RELAXED);
      __atomic_store_n (&undo_pid, pid, __ATOMIC_RELAXED);
    }
  __set_errno (saved);
}

#define WOULD_BLOCK (-1)

/* Applies all of SOPS atomically (called under the lock).  Returns 0, an
   errno value, or WOULD_BLOCK with *BLOCKED = index of the operation that
   cannot proceed; on anything but 0 the set is left unchanged.  */
static int
try_ops (struct fxsem_hdr *h, const struct sembuf *sops, size_t nsops,
         int32_t pid, size_t *blocked)
{
  struct fxsem_sem *s = fxsem_sems (h);
  int32_t *adj = NULL;
  int err = 0;
  size_t i;
  for (i = 0; i < nsops; i++)
    {
      const struct sembuf *op = &sops[i];
      struct fxsem_sem *c = &s[op->sem_num];
      if (op->sem_op == 0)
        {
          if (c->val != 0)
            {
              *blocked = i;
              err = WOULD_BLOCK;
              break;
            }
          continue;
        }
      int32_t nv = c->val + op->sem_op;
      if (nv < 0)
        {
          *blocked = i;
          err = WOULD_BLOCK;
          break;
        }
      if (nv > FXSEM_SEMVMX)
        {
          err = ERANGE;
          break;
        }
      if (op->sem_flg & SEM_UNDO)
        {
          if (adj == NULL)
            {
              int slot = undo_slot (h, pid);
              if (slot < 0)
                {
                  err = ENOSPC;
                  break;
                }
              adj = fxsem_adj (h, slot);
            }
          int32_t a = adj[op->sem_num] - op->sem_op;
          if (a < -FXSEM_SEMAEM - 1 || a > FXSEM_SEMAEM)
            {
              err = ERANGE;
              break;
            }
          adj[op->sem_num] = a;
        }
      c->val = nv;
    }
  if (err != 0)
    {
      /* Roll back operations 0 .. i-1.  */
      while (i-- > 0)
        {
          const struct sembuf *op = &sops[i];
          s[op->sem_num].val -= op->sem_op;
          if ((op->sem_flg & SEM_UNDO) && op->sem_op != 0)
            adj[op->sem_num] += op->sem_op;
        }
      return err;
    }
  for (i = 0; i < nsops; i++)
    s[sops[i].sem_num].pid = pid;
  h->otime = now_realtime ();
  return 0;
}

int
__fxsem_timedop (int semid, struct sembuf *sops, size_t nsops,
                 const struct timespec *timeout)
{
  if (nsops < 1 || semid < 0)
    {
      __set_errno (EINVAL);
      return -1;
    }
  if (nsops > FXSEM_SEMOPM)
    {
      __set_errno (E2BIG);
      return -1;
    }
  if (sops == NULL)
    {
      __set_errno (EFAULT);
      return -1;
    }
  struct timespec deadline;
  if (timeout != NULL)
    {
      if (timeout->tv_sec < 0 || timeout->tv_nsec < 0
          || timeout->tv_nsec >= 1000000000)
        {
          __set_errno (EINVAL);
          return -1;
        }
      __clock_gettime (CLOCK_MONOTONIC, &deadline);
      deadline.tv_nsec += timeout->tv_nsec;
      if (deadline.tv_nsec >= 1000000000)
        {
          deadline.tv_nsec -= 1000000000;
          deadline.tv_sec++;
        }
      if (__builtin_add_overflow (deadline.tv_sec, timeout->tv_sec,
                                  &deadline.tv_sec))
        timeout = NULL;      /* effectively infinite */
    }
  unsigned int max_num = 0;
  bool alter = false, undo = false;
  for (size_t i = 0; i < nsops; i++)
    {
      if (sops[i].sem_num > max_num)
        max_num = sops[i].sem_num;
      if (sops[i].sem_op != 0)
        alter = true;
      if (sops[i].sem_flg & SEM_UNDO)
        undo = true;
    }

  struct semmap *m = acquire (semid);
  if (m == NULL)
    return -1;
  struct fxsem_hdr *h = m->h;
  if (max_num >= (unsigned int) h->nsems)
    {
      cache_put (m);
      __set_errno (EFBIG);
      return -1;
    }
  if (!permitted (h, alter ? 0200 : 0400))
    {
      cache_put (m);
      __set_errno (EACCES);
      return -1;
    }
  int32_t pid = __getpid ();
  if (undo)
    register_undo (pid);

  fxsem_lock (h, (uint32_t) pid);
  bool slept = false, timed_out = false;
  int err;
  for (;;)
    {
      if (h->removed)
        {
          /* Linux: EIDRM for a sleeper, EINVAL for a stale id.  */
          err = slept ? EIDRM : EINVAL;
          break;
        }
      size_t blk = 0;
      err = try_ops (h, sops, nsops, pid, &blk);
      if (err != WOULD_BLOCK)
        break;
      if ((sops[blk].sem_flg & IPC_NOWAIT) || timed_out)
        {
          err = EAGAIN;
          break;
        }
      struct fxsem_sem *s = &fxsem_sems (h)[sops[blk].sem_num];
      bool zero = sops[blk].sem_op == 0;
      if (zero)
        s->zcnt++;
      else
        s->ncnt++;
      h->waiters++;
      uint32_t seq = h->seq;
      fxsem_unlock (h);
      long r = INTERNAL_SYSCALL_CALL (futex, &h->seq, FUTEX_WAIT_BITSET, seq,
                                      timeout != NULL ? &deadline : NULL,
                                      NULL, FUTEX_BITSET_MATCH_ANY);
      fxsem_lock (h, (uint32_t) pid);
      if (zero)
        s->zcnt--;
      else
        s->ncnt--;
      h->waiters--;
      slept = true;
      if (r == -EINTR)
        {
          err = EINTR;
          break;
        }
      if (r == -ETIMEDOUT)
        timed_out = true;   /* one last try, then EAGAIN */
    }
  bool wake = false;
  if (err == 0 && alter)
    wake = fxsem_changed (h);
  bool removed = h->removed;
  fxsem_unlock (h);
  if (wake)
    fxsem_wake_all (h);
  if (removed)
    cache_kill (m);
  cache_put (m);
  if (err != 0)
    {
      __set_errno (err);
      return -1;
    }
  return 0;
}

/* ---- semctl ------------------------------------------------------------------ */

static void
clear_adj (struct fxsem_hdr *h, int semnum)
{
  for (int slot = 0; slot < FXSEM_UNDO_SLOTS; slot++)
    {
      if (h->undo_pid[slot] == 0)
        continue;
      int32_t *adj = fxsem_adj (h, slot);
      if (semnum >= 0)
        adj[semnum] = 0;
      else
        memset (adj, 0, sizeof (int32_t) * h->nsems);
    }
}

int
__fxsem_ctl (int semid, int semnum, int cmd, unsigned long int arg)
{
  __libc_once (once, init_once);
  cmd &= ~FXSEM_IPC_64;
  switch (cmd)
    {
    case IPC_INFO:
    case SEM_INFO:
      {
        struct seminfo *si = (struct seminfo *) arg;
        if (si == NULL)
          {
            __set_errno (EFAULT);
            return -1;
          }
        memset (si, 0, sizeof *si);
        si->semmap = FXSEM_SEMMNI;
        si->semmni = FXSEM_SEMMNI;
        si->semmns = FXSEM_SEMMNI * FXSEM_SEMMSL;
        si->semmnu = FXSEM_SEMMNI * FXSEM_SEMMSL;
        si->semmsl = FXSEM_SEMMSL;
        si->semopm = FXSEM_SEMOPM;
        si->semume = FXSEM_SEMOPM;
        si->semusz = cmd == SEM_INFO ? 0 : 20;
        si->semvmx = FXSEM_SEMVMX;
        si->semaem = cmd == SEM_INFO ? 0 : FXSEM_SEMAEM;
        return 0;   /* highest used index: we do not expose indexes */
      }
    case IPC_RMID:
      {
        if (semid < 0)
          {
            __set_errno (EINVAL);
            return -1;
          }
        struct fxshm_req q = { .op = FXSHM_OP_SEM_RMID, .id = semid };
        struct fxshm_resp r;
        if (__fxshm_call (&q, &r, NULL) < 0)
          return -1;
        struct semmap *m = cache_get (semid);
        if (m != NULL)
          {
            cache_kill (m);
            cache_put (m);
          }
        return 0;
      }
    case IPC_STAT:
    case IPC_SET:
    case GETVAL:
    case GETPID:
    case GETNCNT:
    case GETZCNT:
    case GETALL:
    case SETVAL:
    case SETALL:
      break;
    default:
      /* SEM_STAT, SEM_STAT_ANY (index based listing): not supported.  */
      __set_errno (EINVAL);
      return -1;
    }

  if ((cmd == IPC_STAT || cmd == IPC_SET || cmd == GETALL || cmd == SETALL)
      && arg == 0)
    {
      __set_errno (EFAULT);
      return -1;
    }
  struct semmap *m = acquire (semid);
  if (m == NULL)
    return -1;
  struct fxsem_hdr *h = m->h;
  int32_t nsems = h->nsems;
  bool needs_num = cmd == GETVAL || cmd == GETPID || cmd == GETNCNT
                   || cmd == GETZCNT || cmd == SETVAL;
  int err = 0;
  long int ret = 0;
  if (needs_num && (semnum < 0 || semnum >= nsems))
    err = EINVAL;
  else if (!permitted (h, (cmd == SETVAL || cmd == SETALL) ? 0200
                          : (cmd == IPC_SET) ? 0 : 0400))
    err = EACCES;
  else if (cmd == SETVAL && ((int) arg < 0 || (int) arg > FXSEM_SEMVMX))
    err = ERANGE;
  else if (cmd == SETALL)
    {
      const unsigned short int *v = (const unsigned short int *) arg;
      for (int32_t i = 0; i < nsems; i++)
        if (v[i] > FXSEM_SEMVMX)
          err = ERANGE;
    }
  if (err != 0)
    {
      cache_put (m);
      __set_errno (err);
      return -1;
    }

  int32_t pid = __getpid ();
  struct fxsem_sem *s = fxsem_sems (h);
  bool wake = false;
  fxsem_lock (h, (uint32_t) pid);
  if (h->removed)
    err = EINVAL;
  else
    switch (cmd)
      {
      case IPC_STAT:
        {
          struct semid_ds *buf = (struct semid_ds *) arg;
          memset (buf, 0, sizeof *buf);
          buf->sem_perm.__key = h->key;
          buf->sem_perm.uid = h->uid;
          buf->sem_perm.gid = h->gid;
          buf->sem_perm.cuid = h->cuid;
          buf->sem_perm.cgid = h->cgid;
          buf->sem_perm.mode = h->mode;
          buf->sem_otime = h->otime;
          buf->sem_ctime = h->ctime;
          buf->sem_nsems = h->nsems;
          break;
        }
      case IPC_SET:
        {
          const struct semid_ds *buf = (const struct semid_ds *) arg;
          h->uid = buf->sem_perm.uid;
          h->gid = buf->sem_perm.gid;
          __atomic_store_n (&h->mode, buf->sem_perm.mode & 0777,
                            __ATOMIC_RELAXED);
          h->ctime = now_realtime ();
          break;
        }
      case GETVAL:
        ret = s[semnum].val;
        break;
      case GETPID:
        ret = s[semnum].pid;
        break;
      case GETNCNT:
        ret = s[semnum].ncnt;
        break;
      case GETZCNT:
        ret = s[semnum].zcnt;
        break;
      case GETALL:
        {
          unsigned short int *v = (unsigned short int *) arg;
          for (int32_t i = 0; i < nsems; i++)
            v[i] = (unsigned short int) s[i].val;
          break;
        }
      case SETVAL:
        s[semnum].val = (int) arg;
        s[semnum].pid = pid;
        clear_adj (h, semnum);
        h->ctime = now_realtime ();
        wake = fxsem_changed (h);
        break;
      case SETALL:
        {
          const unsigned short int *v = (const unsigned short int *) arg;
          for (int32_t i = 0; i < nsems; i++)
            {
              s[i].val = v[i];
              s[i].pid = pid;
            }
          clear_adj (h, -1);
          h->ctime = now_realtime ();
          wake = fxsem_changed (h);
          break;
        }
      }
  bool removed = h->removed;
  fxsem_unlock (h);
  if (wake)
    fxsem_wake_all (h);
  if (removed)
    cache_kill (m);
  cache_put (m);
  if (err != 0)
    {
      __set_errno (err);
      return -1;
    }
  return (int) ret;
}
