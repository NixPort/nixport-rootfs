/* fexdroid: memory layout of a userspace SysV semaphore set, shared by the
   patched glibc (patches/glibc/fxshm/fxsem-client.c) and fxshmd.

   Android's app seccomp filter kills processes that issue semget/semop/
   semtimedop/semctl, so every semaphore set is a memfd created by fxshmd and
   mapped MAP_SHARED by every process that uses it.  All state lives in that
   mapping; semop is done entirely in the calling process under a lock word in
   the mapping, blocking waits are FUTEX_WAIT on a sequence word (non-private
   futexes: the mapping is shared between processes).  fxshmd only creates,
   looks up and removes sets, and applies SEM_UNDO adjustments when a process
   goes away (it sees the hangup of the per-process connection).

   This header is the single source of truth: fxshmd.c includes it directly and
   scripts/lib/make-glibc-sysvipc-patch.py copies it into the glibc patch.

   The includer must define, before using the lock helpers:
     static long fxsem_sys_futex (uint32_t *uaddr, int op, uint32_t val,
                                  const struct timespec *rel_timeout);
         -> raw result: >= 0, or -errno
     static int fxsem_sys_alive (int32_t pid);
         -> 0 only when PID certainly does not exist any more (ESRCH)  */
#ifndef FXSEM_LAYOUT_H
#define FXSEM_LAYOUT_H 1

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define FXSEM_MAGIC 0x4d534846u        /* "FHSM" */

/* Limits (reported by semctl IPC_INFO, enforced by semget/semop).  */
#define FXSEM_SEMMSL 32000             /* semaphores per set */
#define FXSEM_SEMMNI 32000             /* sets */
#define FXSEM_SEMOPM 500               /* operations per semop call */
#define FXSEM_SEMVMX 32767             /* maximum semaphore value */
#define FXSEM_SEMAEM FXSEM_SEMVMX      /* maximum SEM_UNDO adjustment */
/* Processes that can hold SEM_UNDO adjustments on one set at the same time.
   A slot whose adjustments are all zero is recycled.  */
#define FXSEM_UNDO_SLOTS 64

#define FXSEM_LOCK_WAITERS 0x80000000u

#ifndef FUTEX_WAIT
# define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
# define FUTEX_WAKE 1
#endif

struct fxsem_sem
{
  int32_t val;        /* semval */
  int32_t pid;        /* sempid: last process that changed it */
  int32_t ncnt;       /* semncnt: waiting for val to increase */
  int32_t zcnt;       /* semzcnt: waiting for val to become 0 */
};

struct fxsem_hdr
{
  uint32_t magic;     /* FXSEM_MAGIC once initialised by fxshmd */
  uint32_t lock;      /* 0 = free, else holder pid [| FXSEM_LOCK_WAITERS] */
  uint32_t seq;       /* futex word, bumped under the lock on every change */
  uint32_t waiters;   /* threads sleeping on seq */
  uint32_t removed;   /* IPC_RMID done: sleepers return EIDRM */
  int32_t id;
  int32_t key;
  int32_t nsems;
  uint32_t uid, gid, cuid, cgid, mode, pad;
  int64_t otime, ctime;
  int32_t undo_pid[FXSEM_UNDO_SLOTS];   /* 0 = free slot */
  /* followed by:
       struct fxsem_sem sems[nsems];
       int32_t adj[FXSEM_UNDO_SLOTS][nsems];   semadj per undo slot  */
};

static inline struct fxsem_sem *
fxsem_sems (struct fxsem_hdr *h)
{
  return (struct fxsem_sem *) (h + 1);
}

static inline int32_t *
fxsem_adj (struct fxsem_hdr *h, int slot)
{
  return (int32_t *) (fxsem_sems (h) + h->nsems) + (size_t) slot * h->nsems;
}

/* Bytes needed for a set of NSEMS semaphores (before page rounding).  */
static inline uint64_t
fxsem_size (int32_t nsems)
{
  return sizeof (struct fxsem_hdr) + (uint64_t) nsems * sizeof (struct fxsem_sem)
         + (uint64_t) FXSEM_UNDO_SLOTS * nsems * sizeof (int32_t);
}

static long fxsem_sys_futex (uint32_t *uaddr, int op, uint32_t val,
                             const struct timespec *rel_timeout);
static int fxsem_sys_alive (int32_t pid);

/* Process-shared lock.  The holder's pid is stored so that a waiter can take
   the lock over when the holder was killed inside the (short, syscall-free)
   critical section: there are no robust futexes (set_robust_list is trapped
   by Android's seccomp filter too).  */
static inline void
fxsem_lock (struct fxsem_hdr *h, uint32_t self)
{
  uint32_t cur = 0;
  if (__atomic_compare_exchange_n (&h->lock, &cur, self, 0, __ATOMIC_ACQUIRE,
                                   __ATOMIC_RELAXED))
    return;
  for (;;)
    {
      if (cur == 0)
        {
          /* We have been contended: keep the waiters bit so that the
             unlock wakes whoever else is sleeping.  */
          if (__atomic_compare_exchange_n (&h->lock, &cur,
                                           self | FXSEM_LOCK_WAITERS, 0,
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
          continue;
        }
      if (!(cur & FXSEM_LOCK_WAITERS))
        {
          if (!__atomic_compare_exchange_n (&h->lock, &cur,
                                            cur | FXSEM_LOCK_WAITERS, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            continue;
          cur |= FXSEM_LOCK_WAITERS;
        }
      struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
      long r = fxsem_sys_futex (&h->lock, FUTEX_WAIT, cur, &ts);
      if (r == -ETIMEDOUT)
        {
          uint32_t owner = cur & ~FXSEM_LOCK_WAITERS;
          if (owner != self && !fxsem_sys_alive ((int32_t) owner)
              && __atomic_compare_exchange_n (&h->lock, &cur,
                                              self | FXSEM_LOCK_WAITERS, 0,
                                              __ATOMIC_ACQUIRE,
                                              __ATOMIC_RELAXED))
            return;   /* holder died with the lock held: take it over */
        }
      cur = __atomic_load_n (&h->lock, __ATOMIC_RELAXED);
    }
}

static inline void
fxsem_unlock (struct fxsem_hdr *h)
{
  if (__atomic_exchange_n (&h->lock, 0, __ATOMIC_RELEASE) & FXSEM_LOCK_WAITERS)
    fxsem_sys_futex (&h->lock, FUTEX_WAKE, 1, NULL);
}

/* Called under the lock after any change sleepers may care about.  Returns
   true if sleepers must be woken (fxsem_wake_all, after unlocking).  */
static inline int
fxsem_changed (struct fxsem_hdr *h)
{
  __atomic_store_n (&h->seq, h->seq + 1, __ATOMIC_RELEASE);
  return h->waiters != 0;
}

static inline void
fxsem_wake_all (struct fxsem_hdr *h)
{
  fxsem_sys_futex (&h->seq, FUTEX_WAKE, 0x7fffffff, NULL);
}

/* Applies and clears the SEM_UNDO adjustments of PID (process exit).  Called
   under the lock; returns fxsem_changed()'s verdict, or 0 if nothing
   changed.  */
static inline int
fxsem_apply_undo (struct fxsem_hdr *h, int32_t pid)
{
  int wake = 0;
  for (int slot = 0; slot < FXSEM_UNDO_SLOTS; slot++)
    {
      if (h->undo_pid[slot] != pid)
        continue;
      int32_t *adj = fxsem_adj (h, slot);
      struct fxsem_sem *s = fxsem_sems (h);
      int changed = 0;
      for (int32_t i = 0; i < h->nsems; i++)
        {
          if (adj[i] == 0)
            continue;
          int64_t v = (int64_t) s[i].val + adj[i];
          if (v < 0)
            v = 0;
          if (v > FXSEM_SEMVMX)
            v = FXSEM_SEMVMX;
          s[i].val = (int32_t) v;
          s[i].pid = pid;
          adj[i] = 0;
          changed = 1;
        }
      h->undo_pid[slot] = 0;
      if (changed)
        wake |= fxsem_changed (h);
    }
  return wake;
}

#endif /* fxsem-layout.h */
