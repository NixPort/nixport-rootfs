/* fexdroid: exercises the userspace SysV shm implementation (glibc patch
 * 0002 + fxshmd).  Built by tools/fxshmd/build.sh, installed in the arm64
 * rootfs as /opt/fexdroid-tests/shmtest, run under qemu by
 * scripts/test-payload-qemu.sh or on the phone.
 *
 *   shmtest all           run every scenario (spawns itself for process B)
 *   shmtest create        create + write a keyed segment, print its id, exit
 *   shmtest read ID       attach ID, verify the pattern, exit  (process B)
 *   shmtest sem           expect msgget/msgctl -> ENOSYS (semaphores: tests/sysvsem)
 *
 * Each check prints "ok - ..." or "FAIL - ..." (TAP-like); exit 1 on failure.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int failures;
static const char *self;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (cond) { printf ("ok - "); printf (__VA_ARGS__); printf ("\n"); }       \
    else { failures++; printf ("FAIL - "); printf (__VA_ARGS__);               \
           printf (" (errno %d %s)\n", errno, strerror (errno)); }             \
    fflush (stdout);                                                           \
  } while (0)

#define KEY 0x46584431   /* 'FXD1' */
#define SIZE 100000      /* not page aligned on purpose */

static void
fill (unsigned char *p, size_t n, unsigned seed)
{
  for (size_t i = 0; i < n; i++)
    p[i] = (unsigned char) (seed + i * 7);
}

static int
verify (const unsigned char *p, size_t n, unsigned seed)
{
  for (size_t i = 0; i < n; i++)
    if (p[i] != (unsigned char) (seed + i * 7))
      return 0;
  return 1;
}

/* Runs "shmtest ARG1 [ARG2]" as a separate process and returns its exit
   status; output goes to our stdout.  */
static int
run_self (const char *arg1, const char *arg2)
{
  char *argv[4] = { (char *) self, (char *) arg1, (char *) arg2, NULL };
  pid_t pid;
  if (posix_spawn (&pid, self, NULL, NULL, argv, environ) != 0)
    return -1;
  int st;
  waitpid (pid, &st, 0);
  return WIFEXITED (st) ? WEXITSTATUS (st) : -1;
}

static int
do_create (void)
{
  int id = shmget (KEY, SIZE, IPC_CREAT | IPC_EXCL | 0600);
  if (id < 0)
    {
      perror ("shmget");
      return 1;
    }
  unsigned char *p = shmat (id, NULL, 0);
  if (p == (void *) -1)
    {
      perror ("shmat");
      return 1;
    }
  fill (p, SIZE, 1);
  shmdt (p);
  printf ("%d\n", id);
  return 0;
}

static int
do_read (int id)
{
  const unsigned char *p = shmat (id, NULL, SHM_RDONLY);
  if (p == (void *) -1)
    {
      perror ("shmat");
      return 1;
    }
  int ok = verify (p, SIZE, 1);
  shmdt (p);
  return ok ? 0 : 2;
}

static int
do_sem (void)
{
  /* SysV semaphores are implemented now (tests/sysvsem/semtest.c).  */
  errno = 0;
  int r = msgget (IPC_PRIVATE, IPC_CREAT | 0600);
  CHECK (r == -1 && errno == ENOSYS, "msgget -> -1/ENOSYS (no syscall issued)");
  errno = 0;
  r = msgctl (0, IPC_RMID, NULL);
  CHECK (r == -1 && errno == ENOSYS, "msgctl -> -1/ENOSYS");
  return failures ? 1 : 0;
}

static int
do_all (void)
{
  struct shmid_ds ds;

  /* 1. Process A (a separate exec'd process) creates a keyed segment and
        writes into it, then exits with everything detached.  The segment
        must survive A's exit.  */
  int rc = run_self ("create", NULL);
  CHECK (rc == 0, "process A created and filled a keyed segment, then exited");

  /* 2. We (unrelated to A) find it by key, and by id.  */
  int id = shmget (KEY, SIZE, 0600);
  CHECK (id >= 0, "shmget(existing key) -> id %d", id);
  errno = 0;
  CHECK (shmget (KEY, SIZE, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST,
         "shmget(IPC_CREAT|IPC_EXCL) on existing key -> EEXIST");
  errno = 0;
  CHECK (shmget (KEY, SIZE + 1, 0600) == -1 && errno == EINVAL,
         "shmget with size > segsz -> EINVAL");
  errno = 0;
  CHECK (shmget (KEY + 1, SIZE, 0600) == -1 && errno == ENOENT,
         "shmget(unknown key, no IPC_CREAT) -> ENOENT");

  CHECK (shmctl (id, IPC_STAT, &ds) == 0 && ds.shm_segsz == SIZE && ds.shm_nattch == 0
         && ds.shm_perm.__key == KEY && ds.shm_perm.mode == 0600,
         "IPC_STAT: segsz=%zu nattch=%lu key=0x%x mode=%o cpid=%d",
         ds.shm_segsz, (unsigned long) ds.shm_nattch, ds.shm_perm.__key, ds.shm_perm.mode, ds.shm_cpid);

  /* 3. Process B (another exec'd process) attaches by id and verifies the
        data A wrote.  */
  char idbuf[16];
  snprintf (idbuf, sizeof idbuf, "%d", id);
  rc = run_self ("read", idbuf);
  CHECK (rc == 0, "unrelated process B attached by id and read A's data");

  /* 4. Attach here, read-write, modify; nattch counts; a second attach
        of the same segment; SHM_RDONLY really is read-only (we don't
        test the SIGSEGV, just the mapping).  */
  unsigned char *p = shmat (id, NULL, 0);
  CHECK (p != (void *) -1 && verify (p, SIZE, 1), "shmat rw + data intact");
  unsigned char *p2 = shmat (id, NULL, SHM_RDONLY);
  CHECK (p2 != (void *) -1 && p2 != p, "second attach (SHM_RDONLY) at a different address");
  p[0] ^= 0xff;
  CHECK (p2[0] == p[0], "both mappings alias the same memory");
  p[0] ^= 0xff;
  CHECK (shmctl (id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 2,
         "IPC_STAT nattch == 2 (got %lu)", (unsigned long) ds.shm_nattch);
  CHECK (shmdt (p2) == 0, "shmdt second mapping");
  errno = 0;
  CHECK (shmdt (p2) == -1 && errno == EINVAL, "shmdt of a detached address -> EINVAL");

  /* 5. shmaddr: attach at a fixed address (must be SHMLBA aligned).  */
  /* Find a free, page-aligned range (works for any VA size / page size).  */
  size_t maplen = (SIZE + 65535) & ~(size_t) 65535;
  void *hint = mmap (NULL, maplen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  munmap (hint, maplen);
  unsigned char *p3 = shmat (id, hint, 0);
  CHECK (p3 == hint || p3 != (void *) -1, "shmat(shmaddr) -> %p", (void *) p3);
  if (p3 != (void *) -1)
    {
      CHECK (p3 == hint && verify (p3, SIZE, 1), "fixed-address mapping shows the data");
      errno = 0;
      CHECK (shmat (id, hint, 0) == (void *) -1 && errno == EINVAL,
             "shmat at an occupied address -> EINVAL");
      shmdt (p3);
    }
  errno = 0;
  CHECK (shmat (id, (char *) hint + 1, 0) == (void *) -1 && errno == EINVAL,
         "shmat at unaligned address without SHM_RND -> EINVAL");
  p3 = shmat (id, (char *) hint + 1, SHM_RND);
  CHECK (p3 == hint, "shmat unaligned + SHM_RND rounds down to %p", (void *) p3);
  if (p3 != (void *) -1)
    shmdt (p3);

  /* 6. fork: the child inherits the attachment and sees the parent's
        writes; nattch counts the child; the child's exit detaches.  */
  p[1] = 0x42;
  pid_t child = fork ();
  if (child == 0)
    {
      int ok = p[1] == 0x42;
      p[2] = 0x43;
      struct shmid_ds cds;
      int st = shmctl (id, IPC_STAT, &cds) == 0 && cds.shm_nattch == 2;
      /* The child can also attach on its own.  */
      unsigned char *cp = shmat (id, NULL, 0);
      int ok2 = cp != (void *) -1 && cp[2] == 0x43;
      _exit (ok && st && ok2 ? 0 : 1);
    }
  int st;
  waitpid (child, &st, 0);
  CHECK (WIFEXITED (st) && WEXITSTATUS (st) == 0,
         "forked child inherited the mapping, saw nattch==2, attached again");
  CHECK (p[2] == 0x43, "parent sees the child's write");
  CHECK (shmctl (id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 1,
         "after child exit nattch back to 1 (got %lu)", (unsigned long) ds.shm_nattch);

  /* 7. IPC_SET.  */
  ds.shm_perm.mode = 0640;
  CHECK (shmctl (id, IPC_SET, &ds) == 0 && shmctl (id, IPC_STAT, &ds) == 0
         && ds.shm_perm.mode == 0640, "IPC_SET mode -> %o", ds.shm_perm.mode);

  /* 8. IPC_RMID with attachments: key released at once, id still usable,
        memory alive until the last detach; then gone.  */
  CHECK (shmctl (id, IPC_RMID, NULL) == 0, "IPC_RMID while attached");
  errno = 0;
  int id2 = shmget (KEY, SIZE, IPC_CREAT | IPC_EXCL | 0600);
  CHECK (id2 >= 0 && id2 != id, "same key creates a NEW segment after RMID (id %d)", id2);
  unsigned char *p4 = shmat (id, NULL, 0);
  CHECK (p4 != (void *) -1 && verify (p4 + 3, SIZE - 3, 22),
         "shmat by id on the RMID'd segment still works (Linux semantics)");
  CHECK (shmctl (id, IPC_STAT, &ds) == 0 && ds.shm_perm.__key == 0 && ds.shm_nattch == 2,
         "RMID'd segment: key now IPC_PRIVATE, nattch %lu", (unsigned long) ds.shm_nattch);
  CHECK (shmdt (p4) == 0 && shmdt (p) == 0, "detach both");
  errno = 0;
  CHECK (shmat (id, NULL, 0) == (void *) -1 && errno == EINVAL,
         "segment destroyed after last detach: shmat -> EINVAL");
  errno = 0;
  CHECK (shmctl (id, IPC_STAT, &ds) == -1 && errno == EINVAL,
         "destroyed segment: IPC_STAT -> EINVAL");

  /* 9. IPC_RMID without attachments destroys immediately; IPC_PRIVATE.  */
  CHECK (shmctl (id2, IPC_RMID, NULL) == 0, "IPC_RMID on the unattached replacement");
  errno = 0;
  CHECK (shmget (KEY, SIZE, 0600) == -1 && errno == ENOENT, "key gone -> ENOENT");
  int pa = shmget (IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  int pb = shmget (IPC_PRIVATE, 4096, IPC_CREAT | 0600);
  CHECK (pa >= 0 && pb >= 0 && pa != pb, "IPC_PRIVATE always creates (ids %d, %d)", pa, pb);
  errno = 0;
  CHECK (shmget (IPC_PRIVATE, 0, IPC_CREAT | 0600) == -1 && errno == EINVAL, "size 0 -> EINVAL");
  shmctl (pa, IPC_RMID, NULL);
  shmctl (pb, IPC_RMID, NULL);

  /* 10. exec releases attachments: process B attaches and exits; the
         daemon must have dropped its attachment (nattch back to 0).  */
  int id3 = shmget (KEY, SIZE, IPC_CREAT | 0600);
  unsigned char *q = shmat (id3, NULL, 0);
  fill (q, SIZE, 1);
  shmdt (q);
  snprintf (idbuf, sizeof idbuf, "%d", id3);
  rc = run_self ("read", idbuf);
  CHECK (rc == 0 && shmctl (id3, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0,
         "process exit releases its attachments (nattch %lu)", (unsigned long) ds.shm_nattch);
  shmctl (id3, IPC_RMID, NULL);

  /* 11. sem/msg -> ENOSYS.  */
  do_sem ();

  printf ("%s\n", failures ? "shmtest: FAILURES" : "shmtest: all ok");
  return failures ? 1 : 0;
}

int
main (int argc, char **argv)
{
  self = argv[0];
  setvbuf (stdout, NULL, _IOLBF, 0);
  if (argc >= 2 && strcmp (argv[1], "create") == 0)
    return do_create ();
  if (argc >= 3 && strcmp (argv[1], "read") == 0)
    return do_read (atoi (argv[2]));
  if (argc >= 2 && strcmp (argv[1], "sem") == 0)
    return do_sem ();
  if (argc >= 2 && strcmp (argv[1], "all") == 0)
    return do_all ();
  fprintf (stderr, "usage: %s all|create|read ID|sem\n", argv[0]);
  return 2;
}
