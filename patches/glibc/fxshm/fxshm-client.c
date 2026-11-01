/* fexdroid: SysV shared memory without the shm* syscalls.
   Copyright (C) 2026 fexdroid contributors.  MIT license (fexdroid); this file
   is added to glibc under the LGPL-2.1-or-later when built into libc.

   Android's app seccomp filter kills a process with SIGSYS when it issues
   shmget/shmat/shmdt/shmctl.  This file implements them on top of memfds
   owned by a per-app registry daemon (fxshmd, tools/fxshmd/fxshmd.c):

     shmget  -> GET      the daemon creates/looks up the segment (memfd)
     shmat   -> ATTACH   the daemon sends a dup of the memfd (SCM_RIGHTS),
                         we mmap it and remember addr -> id
     shmdt   -> DETACH   munmap + tell the daemon
     shmctl  -> STAT/SET/RMID

   The same connection carries the SysV semaphore requests of fxsem-client.c
   (through __fxshm_call).

   Every process keeps a single SOCK_SEQPACKET connection for its lifetime
   (CLOEXEC).  The daemon counts attachments per connection, so exit, exec
   and crashes release them like the kernel would.  After fork() the child
   gets its own connection (atfork handler) and re-registers the attachments
   it inherited, keeping shm_nattch and IPC_RMID semantics right.

   Everything here goes through non-cancellable INTERNAL_SYSCALL_CALLs so a
   pthread_cancel cannot leave the lock held.  */

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <libc-lock.h>
#include <register-atfork.h>
#include <not-cancel.h>
#include <internal-signals.h>
#include <sysdep.h>
#include <fxshm-proto.h>
#include <fxshm-config.h>
#include <fxshm.h>

#ifndef MAP_FIXED_NOREPLACE
# define MAP_FIXED_NOREPLACE 0x100000
#endif

struct attachment
{
  void *addr;
  size_t len;
  int id;
  struct attachment *next;
};

__libc_lock_define_initialized (static, lock);
__libc_once_define (static, once);
static int conn_fd = -1;
static pid_t conn_pid;
static struct attachment *attachments;
/* Bumped whenever the daemon connection is lost: objects cached from the old
   daemon (semaphore set mappings) must not be matched by id any more.  */
unsigned int __fxshm_conn_gen;

/* ---- connection ----------------------------------------------------------- */

static const char *
socket_path (char *buf, size_t len)
{
  const char *env = __libc_secure_getenv (FXSHM_ENV_SOCKET);
  if (env != NULL && *env != '\0')
    return env;
  __snprintf (buf, len, "%s%s/%s", _PATH_TMP, FXSHM_DIR_NAME, FXSHM_SOCK_NAME);
  return buf;
}

static int
try_connect (const char *path)
{
  struct sockaddr_un sa;
  memset (&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  if (strlen (path) >= sizeof sa.sun_path)
    {
      __set_errno (ENAMETOOLONG);
      return -1;
    }
  strcpy (sa.sun_path, path);
  int fd = __socket (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  int r = INTERNAL_SYSCALL_CALL (connect, fd, &sa, sizeof sa);
  if (INTERNAL_SYSCALL_ERROR_P (r))
    {
      __close_nocancel (fd);
      __set_errno (INTERNAL_SYSCALL_ERRNO (r));
      return -1;
    }
  return fd;
}

static int
spawn_child (void *arg)
{
  char *const *argv = arg;
  __execve (argv[0], argv, __environ);
  _exit (127);
}

/* Start fxshmd.  It daemonizes itself and its first process exits 0 only once
   the socket is listening, so on return we can connect right away.  The child
   is created like posix_spawn does (CLONE_VM|CLONE_VFORK|SIGCHLD: qemu-user
   rejects other flag combinations); the application therefore sees one
   SIGCHLD and, if its handler reaps with wait(-1), our wait4 gets ECHILD,
   which is treated as success (the connect retries cover the rest).  */
static int
spawn_daemon (void)
{
  const char *custom = __libc_secure_getenv (FXSHM_ENV_DAEMON);
  char *argv[3];
  if (custom != NULL && *custom != '\0')
    {
      argv[0] = (char *) custom;
      argv[1] = NULL;
    }
  else
    {
      argv[0] = (char *) FXSHM_LDSO;
      argv[1] = (char *) FXSHM_DAEMON;
      argv[2] = NULL;
    }
  const size_t stack_size = 64 * 1024;
  void *stack = __mmap (NULL, stack_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED)
    return -1;
  /* The child shares our memory until it execs: keep signal handlers from
     running in it.  The daemon unblocks everything at startup.  */
  internal_sigset_t oldmask;
  internal_signal_block_all (&oldmask);
  int pid = __clone (spawn_child, (char *) stack + stack_size,
                     CLONE_VM | CLONE_VFORK | SIGCHLD, argv);
  int saved = errno;
  internal_signal_restore_set (&oldmask);
  __munmap (stack, stack_size);
  if (pid < 0)
    {
      __set_errno (saved);
      return -1;
    }
  int status = 0;
  for (;;)
    {
      int r = INTERNAL_SYSCALL_CALL (wait4, pid, &status, 0, NULL);
      if (!INTERNAL_SYSCALL_ERROR_P (r))
        break;
      if (INTERNAL_SYSCALL_ERRNO (r) == ECHILD)
        return 0;
      if (INTERNAL_SYSCALL_ERRNO (r) != EINTR)
        return -1;
    }
  return (WIFEXITED (status) && WEXITSTATUS (status) == 0) ? 0 : -1;
}

static void
sleep_ms (int ms)
{
  struct __timespec64 ts = { .tv_sec = 0, .tv_nsec = (long) ms * 1000000 };
  INTERNAL_SYSCALL_CALL (clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL);
}

static int transact (const struct fxshm_req *q, struct fxshm_resp *r,
                     int *fd_out);

/* After a new connection in a process that already has attachments (fork
   child, or a reconnect), tell the daemon about them.  */
static void
reinherit (void)
{
  for (struct attachment *a = attachments; a != NULL; a = a->next)
    {
      /* Send one INHERIT per id, counting duplicates once.  */
      bool seen = false;
      for (struct attachment *b = attachments; b != a; b = b->next)
        if (b->id == a->id)
          {
            seen = true;
            break;
          }
      if (seen)
        continue;
      uint64_t count = 0;
      for (struct attachment *b = a; b != NULL; b = b->next)
        if (b->id == a->id)
          count++;
      struct fxshm_req q = { .version = FXSHM_PROTO_VERSION,
                             .op = FXSHM_OP_INHERIT, .id = a->id,
                             .size = count, .pid = conn_pid };
      struct fxshm_resp r;
      transact (&q, &r, NULL);   /* the segment may already be gone; fine */
    }
}

/* Called with LOCK held.  Returns 0 with conn_fd usable, else -1/errno.  */
static int
ensure_conn (void)
{
  pid_t pid = __getpid ();
  if (conn_fd >= 0 && conn_pid == pid)
    return 0;
  if (conn_fd >= 0)
    {
      /* Inherited across a fork that bypassed our atfork handler (_Fork,
         raw clone): the parent's connection is not ours to use.  */
      __close_nocancel (conn_fd);
      conn_fd = -1;
    }
  char buf[sizeof ((struct sockaddr_un *) 0)->sun_path];
  const char *path = socket_path (buf, sizeof buf);
  bool spawned = false;
  for (int attempt = 0; attempt < 200; attempt++)
    {
      int fd = try_connect (path);
      if (fd >= 0)
        {
          conn_fd = fd;
          conn_pid = pid;
          if (attachments != NULL)
            reinherit ();
          return 0;
        }
      if (errno != ENOENT && errno != ECONNREFUSED)
        return -1;
      if (!spawned)
        {
          spawned = true;
          spawn_daemon ();   /* failure is not fatal: another process may win */
          continue;
        }
      sleep_ms (10);
    }
  __set_errno (ENOSYS);
  return -1;
}

/* Called with LOCK held.  Returns 0 and fills *R (result >= 0), or -1 with
   errno set (both for transport errors and for r->err).  */
static int
transact (const struct fxshm_req *q, struct fxshm_resp *r, int *fd_out)
{
  if (ensure_conn () < 0)
    return -1;
  for (;;)
    {
      int n = INTERNAL_SYSCALL_CALL (sendto, conn_fd, q, sizeof *q,
                                     MSG_NOSIGNAL, NULL, 0);
      if (!INTERNAL_SYSCALL_ERROR_P (n))
        break;
      if (INTERNAL_SYSCALL_ERRNO (n) == EINTR)
        continue;
      goto lost;
    }
  struct iovec iov = { .iov_base = r, .iov_len = sizeof *r };
  union { struct cmsghdr h; char buf[CMSG_SPACE (sizeof (int))]; } u;
  memset (&u, 0, sizeof u);
  struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = u.buf, .msg_controllen = sizeof u.buf };
  ssize_t n;
  for (;;)
    {
      n = INTERNAL_SYSCALL_CALL (recvmsg, conn_fd, &mh, MSG_CMSG_CLOEXEC);
      if (!INTERNAL_SYSCALL_ERROR_P (n))
        break;
      if (INTERNAL_SYSCALL_ERRNO (n) == EINTR)
        continue;
      goto lost;
    }
  if (n != sizeof *r)
    goto lost;
  int fd = -1;
  for (struct cmsghdr *cm = CMSG_FIRSTHDR (&mh); cm != NULL;
       cm = CMSG_NXTHDR (&mh, cm))
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS
        && cm->cmsg_len == CMSG_LEN (sizeof (int)))
      memcpy (&fd, CMSG_DATA (cm), sizeof (int));
  if (r->result < 0)
    {
      if (fd >= 0)
        __close_nocancel (fd);
      __set_errno (r->err);
      return -1;
    }
  if (fd_out != NULL)
    *fd_out = fd;
  else if (fd >= 0)
    __close_nocancel (fd);
  return 0;

lost:
  /* The daemon went away: drop the connection so the next call restarts
     it.  Segments it held are gone, as after a reboot.  */
  __close_nocancel (conn_fd);
  conn_fd = -1;
  __atomic_fetch_add (&__fxshm_conn_gen, 1, __ATOMIC_RELEASE);
  __set_errno (EIDRM);
  return -1;
}

/* ---- fork handling -------------------------------------------------------- */

static void
fork_prepare (void)
{
  __libc_lock_lock (lock);
}

static void
fork_parent (void)
{
  __libc_lock_unlock (lock);
}

static void
fork_child (void)
{
  __libc_lock_init (lock);
  if (conn_fd >= 0)
    {
      __close_nocancel (conn_fd);
      conn_fd = -1;
    }
  /* Re-register inherited attachments now, before the parent can detach
     and destroy the segment under us.  */
  if (attachments != NULL)
    {
      __libc_lock_lock (lock);
      ensure_conn ();
      __libc_lock_unlock (lock);
    }
}

static void
init_once (void)
{
  /* Registered outside LOCK: prefork handlers run with the atfork lock
     held, and __register_atfork takes that same lock.  */
  __register_atfork (fork_prepare, fork_parent, fork_child, NULL);
}

/* ---- internal entry point for fxsem-client.c ------------------------------ */

/* One request/response on this process's daemon connection.  Q->pid is
   filled in.  Returns 0 (R->result >= 0; *FD_OUT = received fd or -1) or -1
   with errno set.  */
int
__fxshm_call (struct fxshm_req *q, struct fxshm_resp *r, int *fd_out)
{
  __libc_once (once, init_once);
  if (fd_out != NULL)
    *fd_out = -1;
  __libc_lock_lock (lock);
  q->version = FXSHM_PROTO_VERSION;
  q->pid = __getpid ();
  int ret = transact (q, r, fd_out);
  __libc_lock_unlock (lock);
  return ret;
}

/* ---- public entry points (called from shm*.c) ----------------------------- */

int
__fxshm_get (key_t key, size_t size, int shmflg)
{
  __libc_once (once, init_once);
  struct fxshm_req q = { .version = FXSHM_PROTO_VERSION, .op = FXSHM_OP_GET,
                         .key = key, .size = size, .flags = shmflg };
  struct fxshm_resp r;
  __libc_lock_lock (lock);
  q.pid = __getpid ();
  int ret = transact (&q, &r, NULL) < 0 ? -1 : r.result;
  __libc_lock_unlock (lock);
  return ret;
}

static void
detach_locked (int id)
{
  struct fxshm_req q = { .version = FXSHM_PROTO_VERSION, .op = FXSHM_OP_DETACH,
                         .id = id, .pid = __getpid () };
  struct fxshm_resp r;
  int saved = errno;
  transact (&q, &r, NULL);
  __set_errno (saved);
}

void *
__fxshm_at (int shmid, const void *shmaddr, int shmflg)
{
  __libc_once (once, init_once);
  size_t pagesize = __getpagesize ();
  uintptr_t want = 0;
  if (shmaddr != NULL)
    {
      want = (uintptr_t) shmaddr;
      if (shmflg & SHM_RND)
        want &= ~(uintptr_t) (SHMLBA - 1);
      else if (want & (SHMLBA - 1))
        {
          __set_errno (EINVAL);
          return (void *) -1;
        }
    }
  struct attachment *a = malloc (sizeof *a);
  if (a == NULL)
    return (void *) -1;

  struct fxshm_req q = { .version = FXSHM_PROTO_VERSION, .op = FXSHM_OP_ATTACH,
                         .id = shmid, .flags = shmflg };
  struct fxshm_resp r;
  int fd = -1;
  __libc_lock_lock (lock);
  q.pid = __getpid ();
  if (transact (&q, &r, &fd) < 0 || fd < 0)
    {
      if (fd >= 0)
        __close_nocancel (fd);
      __libc_lock_unlock (lock);
      free (a);
      return (void *) -1;
    }
  size_t len = (r.seg.segsz + pagesize - 1) & ~(pagesize - 1);
  int prot = PROT_READ;
  if (!(shmflg & SHM_RDONLY))
    prot |= PROT_WRITE;
  if (shmflg & SHM_EXEC)
    prot |= PROT_EXEC;
  int flags = MAP_SHARED;
  if (want != 0)
    flags |= MAP_FIXED_NOREPLACE;
  void *p = __mmap ((void *) want, len, prot, flags, fd, 0);
  __close_nocancel (fd);
  if (p != MAP_FAILED && want != 0 && p != (void *) want)
    {
      /* Old kernel that ignores MAP_FIXED_NOREPLACE and gave us a hint.  */
      __munmap (p, len);
      p = MAP_FAILED;
      __set_errno (EINVAL);
    }
  if (p == MAP_FAILED)
    {
      int saved = errno;
      detach_locked (shmid);
      __libc_lock_unlock (lock);
      free (a);
      __set_errno (saved == EEXIST ? EINVAL : saved);
      return (void *) -1;
    }
  a->addr = p;
  a->len = len;
  a->id = shmid;
  a->next = attachments;
  attachments = a;
  __libc_lock_unlock (lock);
  return p;
}

int
__fxshm_dt (const void *shmaddr)
{
  __libc_once (once, init_once);
  __libc_lock_lock (lock);
  struct attachment **pp = &attachments;
  while (*pp != NULL && (*pp)->addr != shmaddr)
    pp = &(*pp)->next;
  struct attachment *a = *pp;
  if (a == NULL)
    {
      __libc_lock_unlock (lock);
      __set_errno (EINVAL);
      return -1;
    }
  *pp = a->next;
  __munmap (a->addr, a->len);
  detach_locked (a->id);
  __libc_lock_unlock (lock);
  free (a);
  return 0;
}

int
__fxshm_ctl (int shmid, int cmd, struct shmid_ds *buf)
{
  __libc_once (once, init_once);
  struct fxshm_req q = { .version = FXSHM_PROTO_VERSION, .id = shmid };
  struct fxshm_resp r;
  switch (cmd)
    {
    case IPC_STAT:
    case SHM_LOCK:
    case SHM_UNLOCK:
      q.op = FXSHM_OP_STAT;
      if (cmd == IPC_STAT && buf == NULL)
        {
          __set_errno (EFAULT);
          return -1;
        }
      break;
    case IPC_SET:
      if (buf == NULL)
        {
          __set_errno (EFAULT);
          return -1;
        }
      q.op = FXSHM_OP_SET;
      q.uid = buf->shm_perm.uid;
      q.gid = buf->shm_perm.gid;
      q.mode = buf->shm_perm.mode;
      break;
    case IPC_RMID:
      q.op = FXSHM_OP_RMID;
      break;
    case IPC_INFO:
      {
        /* Static limits, enough for ipcs(1) and sanity checks.  */
        struct shminfo *si = (struct shminfo *) buf;
        if (si == NULL)
          {
            __set_errno (EFAULT);
            return -1;
          }
        memset (si, 0, sizeof *si);
        si->shmmax = (__syscall_ulong_t) 1 << 40;
        si->shmmin = 1;
        si->shmmni = 4096;
        si->shmseg = 4096;
        si->shmall = ((__syscall_ulong_t) 1 << 40) / __getpagesize ();
        return 0;
      }
    default:
      /* SHM_STAT, SHM_STAT_ANY, SHM_INFO: not supported.  */
      __set_errno (EINVAL);
      return -1;
    }
  __libc_lock_lock (lock);
  q.pid = __getpid ();
  int ret = transact (&q, &r, NULL);
  __libc_lock_unlock (lock);
  if (ret < 0)
    return -1;
  if (cmd == IPC_STAT)
    {
      memset (buf, 0, sizeof *buf);
      buf->shm_perm.__key = r.seg.key;
      buf->shm_perm.uid = r.seg.uid;
      buf->shm_perm.gid = r.seg.gid;
      buf->shm_perm.cuid = r.seg.cuid;
      buf->shm_perm.cgid = r.seg.cgid;
      buf->shm_perm.mode = r.seg.mode;
      buf->shm_segsz = r.seg.segsz;
      buf->shm_atime = r.seg.atime;
      buf->shm_dtime = r.seg.dtime;
      buf->shm_ctime = r.seg.ctime;
      buf->shm_cpid = r.seg.cpid;
      buf->shm_lpid = r.seg.lpid;
      buf->shm_nattch = r.seg.nattch;
    }
  return 0;
}
