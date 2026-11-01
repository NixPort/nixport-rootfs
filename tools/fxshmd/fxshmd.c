/* fxshmd: per-app SysV shared memory registry for fexdroid.
 *
 * Android's app seccomp filter kills any process that issues shmget/shmat/
 * shmdt/shmctl (SIGSYS), so the fexdroid glibc implements them in userspace:
 * every segment is a memfd owned by this daemon; clients (glibc) talk to it
 * over a unix SOCK_SEQPACKET socket and receive a duplicate of the memfd via
 * SCM_RIGHTS, which they mmap themselves.  This gives Linux semantics that a
 * pure client-side scheme cannot:
 *   - segments outlive their creator (X client exits, X server keeps using it)
 *   - key -> id lookup across unrelated processes
 *   - shm_nattch and "destroy after IPC_RMID + last detach": every process
 *     keeps one connection for its lifetime (CLOEXEC), so when it exits, execs
 *     or crashes the daemon sees the hangup and drops its attachments.
 *
 * SysV semaphores use the same daemon: each set is a memfd (layout in
 * fxsem-layout.h) that clients map MAP_SHARED and operate on directly with a
 * process-shared lock + futexes.  The daemon only creates/looks up/removes
 * sets and, when a process that used SEM_UNDO hangs up, applies its
 * adjustments (Linux exit_sem semantics).
 *
 * Wire format: tools/fxshmd/fxshm-proto.h (also copied into the glibc patch).
 *
 * Lifecycle: started on demand by the first glibc client (or by the app).
 * It takes an flock() on <dir>/lock, replaces a stale socket, binds, forks and
 * lets the intermediate process exit 0 only once the socket is listening, so
 * the spawner can connect immediately.  Losing the lock means another daemon
 * is up: exit 0 and let the client connect to that one.
 *
 * Usage: fxshmd [--dir DIR] [--idle-exit SECONDS] [--foreground]
 *   DIR defaults to $TMPDIR/.fxshm (TMPDIR is $FXD_ROOT/tmp in the app env);
 *   --idle-exit N: exit after N seconds with no segments and no clients
 *   (0 = never, the default).
 *
 * License: MIT (fexdroid).  Written from scratch; not derived from Winlator's
 * android_sysvshm or Termux's libandroid-shmem.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#include "fxshm-proto.h"
#include "fxsem-layout.h"

#define MAX_CONNS 1024
/* Client mmaps round up to the page size; back the whole last page.  */
#define PAGE_ROUND(x) (((x) + (uint64_t) getpagesize () - 1) & ~((uint64_t) getpagesize () - 1))

struct conn;

struct att
{
  struct conn *conn;
  uint64_t count;
  struct att *next;
};

struct seg
{
  int id;
  int fd;             /* the memfd; -1 once destroyed */
  bool rmid;          /* IPC_RMID seen: destroy when nattch drops to 0 */
  struct fxshm_seg info;
  struct att *atts;
  struct seg *next;
};

struct conn
{
  int fd;             /* -1 = free slot */
  pid_t pid;          /* last pid seen in a request (informational) */
  pid_t undo_pid;     /* SEM_UNDO registration: apply on hangup (0 = none) */
};

/* A SysV semaphore set: the memfd plus our own mapping of it.  */
struct semset
{
  int id;
  int fd;
  int32_t key;        /* IPC_PRIVATE for private sets */
  uint64_t size;      /* bytes mapped (page rounded) */
  struct fxsem_hdr *h;
  struct semset *next;
};

static struct seg *segs;
static struct semset *semsets;
static int next_sem_id = 1;
static struct conn conns[MAX_CONNS];
static int next_id = 1;
static int listen_fd = -1;
static char sock_path[sizeof ((struct sockaddr_un *) 0)->sun_path];
static bool verbose;

static void
dlog (const char *fmt, ...)
{
  if (!verbose)
    return;
  va_list ap;
  va_start (ap, fmt);
  fprintf (stderr, "fxshmd: ");
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
}

static int64_t
now (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  return ts.tv_sec;
}

/* ---- segment table -------------------------------------------------------- */

static struct seg *
seg_by_id (int id)
{
  for (struct seg *s = segs; s != NULL; s = s->next)
    if (s->id == id)
      return s;
  return NULL;
}

static struct seg *
seg_by_key (int32_t key)
{
  for (struct seg *s = segs; s != NULL; s = s->next)
    if (s->info.key == key)
      return s;
  return NULL;
}

static void
seg_destroy (struct seg *s)
{
  dlog ("destroy id=%d key=%d segsz=%llu", s->id, s->info.key,
        (unsigned long long) s->info.segsz);
  struct seg **pp = &segs;
  while (*pp != s)
    pp = &(*pp)->next;
  *pp = s->next;
  for (struct att *a = s->atts; a != NULL;)
    {
      struct att *n = a->next;
      free (a);
      a = n;
    }
  if (s->fd >= 0)
    close (s->fd);
  free (s);
}

static void
seg_maybe_destroy (struct seg *s)
{
  if (s->rmid && s->info.nattch == 0)
    seg_destroy (s);
}

static struct att *
att_find (struct seg *s, struct conn *c)
{
  for (struct att *a = s->atts; a != NULL; a = a->next)
    if (a->conn == c)
      return a;
  return NULL;
}

static int
att_add (struct seg *s, struct conn *c, uint64_t count)
{
  struct att *a = att_find (s, c);
  if (a == NULL)
    {
      a = calloc (1, sizeof *a);
      if (a == NULL)
        return -1;
      a->conn = c;
      a->next = s->atts;
      s->atts = a;
    }
  a->count += count;
  s->info.nattch += count;
  return 0;
}

/* Returns false if this connection has no attachment to S.  */
static bool
att_remove (struct seg *s, struct conn *c)
{
  struct att **pp = &s->atts;
  for (; *pp != NULL; pp = &(*pp)->next)
    if ((*pp)->conn == c)
      {
        struct att *a = *pp;
        a->count--;
        s->info.nattch--;
        if (a->count == 0)
          {
            *pp = a->next;
            free (a);
          }
        return true;
      }
  return false;
}

/* The client process went away (exit, exec, crash): drop everything it had
   attached, as the kernel does for real segments.  */
static void
conn_release_all (struct conn *c)
{
  for (struct seg *s = segs; s != NULL;)
    {
      struct seg *next = s->next;
      struct att *a = att_find (s, c);
      if (a != NULL)
        {
          while (att_remove (s, c))
            ;
          seg_maybe_destroy (s);
        }
      s = next;
    }
}

/* ---- request handling ----------------------------------------------------- */

static int
check_perm (const struct seg *s, int want)
{
  /* Single-uid sandbox: the owner bits are all that can ever apply.  */
  if ((want & 0400) && !(s->info.mode & 0400))
    return -1;
  if ((want & 0200) && !(s->info.mode & 0200))
    return -1;
  return 0;
}

static int
op_get (struct conn *c, const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct seg *s = NULL;
  if (q->key != IPC_PRIVATE)
    {
      s = seg_by_key (q->key);
      if (s != NULL)
        {
          if ((q->flags & IPC_CREAT) && (q->flags & IPC_EXCL))
            return EEXIST;
          if (q->size > s->info.segsz)
            return EINVAL;
          if (check_perm (s, q->flags & 0777) < 0)
            return EACCES;
          r->result = s->id;
          r->seg = s->info;
          return 0;
        }
      if (!(q->flags & IPC_CREAT))
        return ENOENT;
    }
  if (q->size == 0 || q->size > (uint64_t) 1 << 40)
    return EINVAL;

  s = calloc (1, sizeof *s);
  if (s == NULL)
    return ENOMEM;
  char name[32];
  snprintf (name, sizeof name, "fxshm:%d", next_id);
  s->fd = memfd_create (name, MFD_CLOEXEC);
  if (s->fd < 0)
    {
      int e = errno;
      free (s);
      return e;
    }
  if (ftruncate (s->fd, (off_t) PAGE_ROUND (q->size)) < 0)
    {
      int e = errno;
      close (s->fd);
      free (s);
      return e;
    }
  s->id = next_id++;
  if (next_id < 0)
    next_id = 1;
  s->info.key = q->key;
  s->info.uid = s->info.cuid = getuid ();
  s->info.gid = s->info.cgid = getgid ();
  s->info.mode = q->flags & 0777;
  s->info.segsz = q->size;
  s->info.ctime = now ();
  s->info.cpid = q->pid;
  s->next = segs;
  segs = s;
  c->pid = q->pid;
  dlog ("create id=%d key=%d size=%llu pid=%d", s->id, s->info.key,
        (unsigned long long) s->info.segsz, q->pid);
  r->result = s->id;
  r->seg = s->info;
  return 0;
}

static int
op_attach (struct conn *c, const struct fxshm_req *q, struct fxshm_resp *r,
           int *fd_out)
{
  struct seg *s = seg_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  if (check_perm (s, (q->flags & SHM_RDONLY) ? 0400 : 0600) < 0)
    return EACCES;
  if (att_add (s, c, 1) < 0)
    return ENOMEM;
  s->info.atime = now ();
  s->info.lpid = q->pid;
  c->pid = q->pid;
  r->result = 0;
  r->seg = s->info;
  *fd_out = s->fd;
  return 0;
}

static int
op_detach (struct conn *c, const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct seg *s = seg_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  if (!att_remove (s, c))
    return EINVAL;
  s->info.dtime = now ();
  s->info.lpid = q->pid;
  r->result = 0;
  seg_maybe_destroy (s);
  return 0;
}

static int
op_inherit (struct conn *c, const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct seg *s = seg_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  if (q->size == 0)
    return EINVAL;
  if (att_add (s, c, q->size) < 0)
    return ENOMEM;
  r->result = 0;
  return 0;
}

static int
op_stat (const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct seg *s = seg_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  r->result = 0;
  r->seg = s->info;
  return 0;
}

static int
op_set (const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct seg *s = seg_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  s->info.uid = q->uid;
  s->info.gid = q->gid;
  s->info.mode = q->mode & 0777;
  s->info.ctime = now ();
  r->result = 0;
  return 0;
}

static int
op_rmid (const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct seg *s = seg_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  /* Linux: the key is released immediately (a new shmget with the same key
     creates a new segment), the memory lives until the last detach.  */
  s->rmid = true;
  s->info.key = IPC_PRIVATE;
  s->info.ctime = now ();
  r->result = 0;
  seg_maybe_destroy (s);
  return 0;
}

/* ---- semaphore sets ------------------------------------------------------- */

static long
fxsem_sys_futex (uint32_t *uaddr, int op, uint32_t val,
                 const struct timespec *rel_timeout)
{
  long r = syscall (SYS_futex, uaddr, op, val, rel_timeout, NULL, 0);
  return r < 0 ? -errno : r;
}

static int
fxsem_sys_alive (int32_t pid)
{
  return !(kill (pid, 0) < 0 && errno == ESRCH);
}

static struct semset *
semset_by_id (int id)
{
  for (struct semset *s = semsets; s != NULL; s = s->next)
    if (s->id == id)
      return s;
  return NULL;
}

static struct semset *
semset_by_key (int32_t key)
{
  for (struct semset *s = semsets; s != NULL; s = s->next)
    if (s->key == key)
      return s;
  return NULL;
}

static int
op_sem_get (struct conn *c, const struct fxshm_req *q, struct fxshm_resp *r,
            int *fd_out)
{
  struct semset *s = NULL;
  int64_t nsems = (int64_t) q->size;
  if (nsems < 0 || nsems > FXSEM_SEMMSL)
    return EINVAL;
  if (q->key != IPC_PRIVATE)
    {
      s = semset_by_key (q->key);
      if (s != NULL)
        {
          if ((q->flags & IPC_CREAT) && (q->flags & IPC_EXCL))
            return EEXIST;
          if (nsems > s->h->nsems)
            return EINVAL;
          uint32_t mode = __atomic_load_n (&s->h->mode, __ATOMIC_RELAXED);
          if (((q->flags & 0400) && !(mode & 0400))
              || ((q->flags & 0200) && !(mode & 0200)))
            return EACCES;
          r->result = s->id;
          r->seg.segsz = s->size;
          *fd_out = s->fd;
          return 0;
        }
      if (!(q->flags & IPC_CREAT))
        return ENOENT;
    }
  if (nsems == 0)
    return EINVAL;
  size_t count = 0;
  for (struct semset *t = semsets; t != NULL; t = t->next)
    count++;
  if (count >= FXSEM_SEMMNI)
    return ENOSPC;

  s = calloc (1, sizeof *s);
  if (s == NULL)
    return ENOMEM;
  char name[32];
  snprintf (name, sizeof name, "fxsem:%d", next_sem_id);
  s->fd = memfd_create (name, MFD_CLOEXEC);
  if (s->fd < 0)
    {
      int e = errno;
      free (s);
      return e;
    }
  s->size = PAGE_ROUND (fxsem_size ((int32_t) nsems));
  void *p = MAP_FAILED;
  if (ftruncate (s->fd, (off_t) s->size) == 0)
    p = mmap (NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
  if (p == MAP_FAILED)
    {
      int e = errno;
      close (s->fd);
      free (s);
      return e;
    }
  s->h = p;
  s->id = next_sem_id++;
  if (next_sem_id <= 0)
    next_sem_id = 1;
  s->key = q->key;
  struct fxsem_hdr *h = s->h;
  h->id = s->id;
  h->key = q->key;
  h->nsems = (int32_t) nsems;
  h->uid = h->cuid = getuid ();
  h->gid = h->cgid = getgid ();
  h->mode = q->flags & 0777;
  h->ctime = now ();
  /* Publish last: clients check the magic before trusting the mapping.  */
  __atomic_store_n (&h->magic, FXSEM_MAGIC, __ATOMIC_RELEASE);
  s->next = semsets;
  semsets = s;
  c->pid = q->pid;
  dlog ("sem create id=%d key=%d nsems=%d pid=%d", s->id, s->key, h->nsems,
        q->pid);
  r->result = s->id;
  r->seg.segsz = s->size;
  *fd_out = s->fd;
  return 0;
}

static int
op_sem_open (const struct fxshm_req *q, struct fxshm_resp *r, int *fd_out)
{
  struct semset *s = semset_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  r->result = 0;
  r->seg.segsz = s->size;
  *fd_out = s->fd;
  return 0;
}

static int
op_sem_rmid (const struct fxshm_req *q, struct fxshm_resp *r)
{
  struct semset *s = semset_by_id (q->id);
  if (s == NULL)
    return EINVAL;
  struct fxsem_hdr *h = s->h;
  fxsem_lock (h, (uint32_t) getpid ());
  h->removed = 1;
  memset (h->undo_pid, 0, sizeof h->undo_pid);
  int wake = fxsem_changed (h);
  fxsem_unlock (h);
  if (wake)
    fxsem_wake_all (h);
  dlog ("sem rmid id=%d key=%d", s->id, s->key);
  /* Clients keep their mappings (and so the memory) until they notice.  */
  struct semset **pp = &semsets;
  while (*pp != s)
    pp = &(*pp)->next;
  *pp = s->next;
  munmap (s->h, s->size);
  close (s->fd);
  free (s);
  r->result = 0;
  return 0;
}

static int
op_sem_undo (struct conn *c, const struct fxshm_req *q, struct fxshm_resp *r)
{
  if (q->pid <= 0)
    return EINVAL;
  c->undo_pid = q->pid;
  c->pid = q->pid;
  r->result = 0;
  return 0;
}

/* A connection that registered for SEM_UNDO went away.  Linux keeps semadj
   across execve, so if the same pid already registered again on a new
   connection (the exec'd image uses semaphores too) nothing is applied.  */
static void
conn_apply_undo (struct conn *c)
{
  pid_t pid = c->undo_pid;
  c->undo_pid = 0;
  if (pid <= 0)
    return;
  for (size_t i = 0; i < MAX_CONNS; i++)
    if (conns[i].fd >= 0 && &conns[i] != c && conns[i].undo_pid == pid)
      return;
  for (struct semset *s = semsets; s != NULL; s = s->next)
    {
      fxsem_lock (s->h, (uint32_t) getpid ());
      int wake = fxsem_apply_undo (s->h, pid);
      fxsem_unlock (s->h);
      if (wake)
        fxsem_wake_all (s->h);
    }
  dlog ("applied SEM_UNDO of pid %d", pid);
}

static void
send_resp (int fd, const struct fxshm_resp *r, int pass_fd)
{
  struct iovec iov = { .iov_base = (void *) r, .iov_len = sizeof *r };
  union { struct cmsghdr h; char buf[CMSG_SPACE (sizeof (int))]; } u;
  struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
  if (pass_fd >= 0)
    {
      memset (&u, 0, sizeof u);
      mh.msg_control = u.buf;
      mh.msg_controllen = sizeof u.buf;
      struct cmsghdr *cm = CMSG_FIRSTHDR (&mh);
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type = SCM_RIGHTS;
      cm->cmsg_len = CMSG_LEN (sizeof (int));
      memcpy (CMSG_DATA (cm), &pass_fd, sizeof (int));
    }
  ssize_t n;
  do
    n = sendmsg (fd, &mh, MSG_NOSIGNAL);
  while (n < 0 && errno == EINTR);
}

/* Returns false when the connection must be closed.  */
static bool
handle_conn (struct conn *c)
{
  struct fxshm_req q;
  ssize_t n;
  do
    n = recv (c->fd, &q, sizeof q, 0);
  while (n < 0 && errno == EINTR);
  if (n <= 0)
    return false;              /* hangup or error */
  struct fxshm_resp r;
  memset (&r, 0, sizeof r);
  int pass_fd = -1;
  int e;
  if ((size_t) n != sizeof q || q.version != FXSHM_PROTO_VERSION)
    e = EPROTO;
  else
    switch (q.op)
      {
      case FXSHM_OP_PING:    r.result = 0; e = 0; break;
      case FXSHM_OP_GET:     e = op_get (c, &q, &r); break;
      case FXSHM_OP_ATTACH:  e = op_attach (c, &q, &r, &pass_fd); break;
      case FXSHM_OP_DETACH:  e = op_detach (c, &q, &r); break;
      case FXSHM_OP_INHERIT: e = op_inherit (c, &q, &r); break;
      case FXSHM_OP_STAT:    e = op_stat (&q, &r); break;
      case FXSHM_OP_SET:     e = op_set (&q, &r); break;
      case FXSHM_OP_RMID:    e = op_rmid (&q, &r); break;
      case FXSHM_OP_SEM_GET: e = op_sem_get (c, &q, &r, &pass_fd); break;
      case FXSHM_OP_SEM_OPEN: e = op_sem_open (&q, &r, &pass_fd); break;
      case FXSHM_OP_SEM_RMID: e = op_sem_rmid (&q, &r); break;
      case FXSHM_OP_SEM_UNDO: e = op_sem_undo (c, &q, &r); break;
      default:               e = EINVAL; break;
      }
  if (e != 0)
    {
      r.result = -1;
      r.err = e;
      pass_fd = -1;
    }
  send_resp (c->fd, &r, pass_fd);
  return true;
}

/* ---- startup -------------------------------------------------------------- */

static void
close_inherited_fds (int keep)
{
  /* We are spawned from arbitrary processes (the X server, guests ...) and must not
     keep their pipes/sockets alive.  */
  DIR *d = opendir ("/proc/self/fd");
  if (d == NULL)
    {
      for (int fd = 3; fd < 1024; fd++)
        if (fd != keep)
          close (fd);
      return;
    }
  int dfd = dirfd (d);
  struct dirent *e;
  while ((e = readdir (d)) != NULL)
    {
      int fd = atoi (e->d_name);
      if (fd > 2 && fd != dfd && fd != keep)
        close (fd);
    }
  closedir (d);
}

static void
usage (void)
{
  fprintf (stderr, "usage: fxshmd [--dir DIR] [--idle-exit SECONDS] [--foreground] [--verbose]\n");
  exit (2);
}

int
main (int argc, char **argv)
{
  const char *dir = NULL;
  int idle_exit = 0;
  bool foreground = false;
  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--dir") == 0 && i + 1 < argc)
        dir = argv[++i];
      else if (strcmp (argv[i], "--idle-exit") == 0 && i + 1 < argc)
        idle_exit = atoi (argv[++i]);
      else if (strcmp (argv[i], "--foreground") == 0)
        foreground = true;
      else if (strcmp (argv[i], "--verbose") == 0)
        verbose = true;
      else
        usage ();
    }
  char dirbuf[sizeof sock_path];
  if (dir == NULL)
    {
      const char *tmp = getenv ("TMPDIR");
      if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
      snprintf (dirbuf, sizeof dirbuf, "%s/%s", tmp, FXSHM_DIR_NAME);
      dir = dirbuf;
    }
  if (snprintf (sock_path, sizeof sock_path, "%s/%s", dir, FXSHM_SOCK_NAME)
      >= (int) sizeof sock_path)
    {
      fprintf (stderr, "fxshmd: socket path too long: %s\n", dir);
      return 1;
    }

  /* The spawner (glibc) execs us with every signal blocked and handlers reset;
     the app may start us from anywhere.  Normalise both.  */
  sigset_t none;
  sigemptyset (&none);
  sigprocmask (SIG_SETMASK, &none, NULL);
  signal (SIGPIPE, SIG_IGN);
  signal (SIGHUP, SIG_IGN);
  close_inherited_fds (-1);
  if (!foreground)
    {
      int nul = open ("/dev/null", O_RDWR | O_CLOEXEC);
      if (nul >= 0)
        {
          dup2 (nul, 0);
          dup2 (nul, 1);
          if (!verbose)
            dup2 (nul, 2);
          close (nul);
        }
    }

  mkdir (dir, 0700);
  char lock_path[sizeof sock_path + 8];
  snprintf (lock_path, sizeof lock_path, "%s/%s", dir, FXSHM_LOCK_NAME);
  int lock_fd = open (lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd < 0)
    {
      fprintf (stderr, "fxshmd: cannot open %s: %s\n", lock_path, strerror (errno));
      return 1;
    }
  if (flock (lock_fd, LOCK_EX | LOCK_NB) < 0)
    {
      /* Another daemon owns the registry; the client will connect to it.  */
      dlog ("another fxshmd holds %s, exiting", lock_path);
      return 0;
    }

  listen_fd = socket (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (listen_fd < 0)
    {
      fprintf (stderr, "fxshmd: socket: %s\n", strerror (errno));
      return 1;
    }
  struct sockaddr_un sa;
  memset (&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strcpy (sa.sun_path, sock_path);
  unlink (sock_path);          /* stale socket from a dead daemon (we hold the lock) */
  if (bind (listen_fd, (struct sockaddr *) &sa, sizeof sa) < 0
      || listen (listen_fd, 64) < 0)
    {
      fprintf (stderr, "fxshmd: bind/listen %s: %s\n", sock_path, strerror (errno));
      return 1;
    }
  chmod (sock_path, 0600);

  if (!foreground)
    {
      /* Readiness handshake: the intermediate exits 0 only after listen().  */
      pid_t pid = fork ();
      if (pid < 0)
        {
          fprintf (stderr, "fxshmd: fork: %s\n", strerror (errno));
          return 1;
        }
      if (pid > 0)
        _exit (0);
      setsid ();
    }
  dlog ("listening on %s (pid %d)", sock_path, getpid ());

  for (size_t i = 0; i < MAX_CONNS; i++)
    conns[i].fd = -1;
  /* Semaphore ids of different daemon instances should not collide: a
     process that still maps a set of a dead daemon must not confuse it with
     a new set (see fxsem-client.c).  */
  next_sem_id = 1 + (int) ((((uint32_t) getpid () * 2654435761u)
                            ^ (uint32_t) now ()) & 0x3fff) * 65536;
  static struct pollfd pfds[MAX_CONNS + 1];
  int64_t idle_since = now ();
  bool was_idle = true;
  for (;;)
    {
      size_t n = 0;
      pfds[n].fd = listen_fd;
      pfds[n].events = POLLIN;
      n++;
      size_t nconns = 0;
      for (size_t i = 0; i < MAX_CONNS; i++)
        if (conns[i].fd >= 0)
          {
            pfds[n].fd = conns[i].fd;
            pfds[n].events = POLLIN;
            n++;
            nconns++;
          }
      int timeout = -1;
      if (idle_exit > 0 && nconns == 0 && segs == NULL && semsets == NULL)
        {
          int64_t left = idle_since + idle_exit - now ();
          if (left <= 0)
            {
              dlog ("idle for %ds, exiting", idle_exit);
              unlink (sock_path);
              break;
            }
          timeout = (int) left * 1000;
        }
      int rc = poll (pfds, n, timeout);
      if (rc < 0)
        {
          if (errno == EINTR)
            continue;
          break;
        }
      if (pfds[0].revents & POLLIN)
        {
          int fd = accept4 (listen_fd, NULL, NULL, SOCK_CLOEXEC);
          if (fd >= 0)
            {
              size_t i;
              for (i = 0; i < MAX_CONNS; i++)
                if (conns[i].fd < 0)
                  break;
              if (i == MAX_CONNS)
                close (fd);
              else
                {
                  conns[i].fd = fd;
                  conns[i].pid = 0;
                  conns[i].undo_pid = 0;
                }
            }
        }
      for (size_t k = 1; k < n; k++)
        {
          if (pfds[k].revents == 0)
            continue;
          struct conn *c = NULL;
          for (size_t i = 0; i < MAX_CONNS; i++)
            if (conns[i].fd == pfds[k].fd)
              {
                c = &conns[i];
                break;
              }
          if (c == NULL)
            continue;
          bool keep = (pfds[k].revents & POLLIN) ? handle_conn (c) : false;
          if (!keep)
            {
              dlog ("client fd=%d pid=%d gone", c->fd, c->pid);
              close (c->fd);
              c->fd = -1;
              conn_release_all (c);
              conn_apply_undo (c);
            }
        }
      bool idle = segs == NULL && semsets == NULL;
      for (size_t i = 0; idle && i < MAX_CONNS; i++)
        if (conns[i].fd >= 0)
          idle = false;
      if (idle && !was_idle)
        idle_since = now ();
      was_idle = idle;
    }
  return 0;
}
