/* fexdroid: exercises the userspace SysV semaphores (glibc patch 0002/0003 +
 * fxshmd, see NOTES.md N-025).  Built by tools/fxshmd/build.sh, installed in
 * the arm64 rootfs as /opt/fexdroid-tests/semtest, run under qemu by
 * scripts/test-payload-qemu.sh or on the phone.
 *
 *   semtest all              run every scenario (spawns itself for the
 *                            "unrelated process" cases)
 *   semtest post KEY         helper: wait for a sleeper on KEY, then +1
 *   semtest hold KEY MODE    helper: take KEY with SEM_UNDO, then exit
 *                            (MODE=exit) or SIGKILL itself (MODE=kill)
 *
 * Each check prints "ok - ..." or "FAIL - ..." (TAP-like); exit 1 on failure.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static int failures;
static const char *self;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    int e_ = errno;                                                            \
    if (cond) { printf ("ok - "); printf (__VA_ARGS__); printf ("\n"); }       \
    else { failures++; printf ("FAIL - "); printf (__VA_ARGS__);               \
           printf (" (errno %d %s)\n", e_, strerror (e_)); }                   \
    fflush (stdout);                                                           \
  } while (0)

#define KEY_A 0x46585341   /* 'FXSA' */
#define KEY_B 0x46585342
#define KEY_C 0x46585343

union semun
{
  int val;
  struct semid_ds *buf;
  unsigned short *array;
  struct seminfo *__buf;
};

static double
now_s (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
sleep_ms (int ms)
{
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep (&ts, NULL);
}

/* Removes a leftover set with KEY from an earlier run.  */
static void
drop_key (key_t key)
{
  int id = semget (key, 0, 0);
  if (id >= 0)
    semctl (id, 0, IPC_RMID);
}

static pid_t
spawn_self (const char *a1, const char *a2, const char *a3)
{
  char *argv[5] = { (char *) self, (char *) a1, (char *) a2, (char *) a3, NULL };
  pid_t pid;
  if (posix_spawn (&pid, self, NULL, NULL, argv, environ) != 0)
    return -1;
  return pid;
}

static int
wait_status (pid_t pid)
{
  int st;
  if (waitpid (pid, &st, 0) < 0)
    return -1;
  if (WIFSIGNALED (st))
    return 128 + WTERMSIG (st);
  return WIFEXITED (st) ? WEXITSTATUS (st) : -1;
}

/* Polls until semctl(CMD) on semaphore 0 of ID returns WANT (or ~5 s).  */
static int
wait_for (int id, int cmd, int want)
{
  for (int i = 0; i < 500; i++)
    {
      if (semctl (id, 0, cmd) == want)
        return 1;
      sleep_ms (10);
    }
  return 0;
}

/* ---- helpers run as separate processes ---------------------------------- */

static int
do_post (key_t key)
{
  int id = semget (key, 0, 0);
  if (id < 0)
    return 2;
  if (!wait_for (id, GETNCNT, 1))   /* the parent is asleep in semop */
    return 3;
  struct sembuf sb = { 0, +1, 0 };
  return semop (id, &sb, 1) == 0 ? 0 : 4;
}

static int
do_hold (key_t key, const char *mode)
{
  int id = semget (key, 0, 0);
  if (id < 0)
    return 2;
  struct sembuf sb = { 0, -1, SEM_UNDO | IPC_NOWAIT };
  if (semop (id, &sb, 1) != 0)
    return 3;
  if (strcmp (mode, "kill") == 0)
    kill (getpid (), SIGKILL);
  return 0;                /* exit while holding it */
}

/* ---- scenarios ------------------------------------------------------------- */

static void
test_basic (void)
{
  drop_key (KEY_A);
  int id = semget (KEY_A, 3, IPC_CREAT | IPC_EXCL | 0600);
  CHECK (id >= 0, "semget(KEY, 3, IPC_CREAT|IPC_EXCL) creates a set (id %d)", id);
  errno = 0;
  int r = semget (KEY_A, 3, IPC_CREAT | IPC_EXCL | 0600);
  CHECK (r == -1 && errno == EEXIST, "second IPC_CREAT|IPC_EXCL -> EEXIST");
  CHECK (semget (KEY_A, 1, 0600) == id, "semget without EXCL returns the same id");
  CHECK (semget (KEY_A, 0, 0) == id, "semget(nsems=0) looks up the existing set");
  errno = 0;
  r = semget (KEY_A, 4, 0600);
  CHECK (r == -1 && errno == EINVAL, "semget with more nsems than the set -> EINVAL");
  errno = 0;
  r = semget (KEY_B + 100, 1, 0600);
  CHECK (r == -1 && errno == ENOENT, "semget of a missing key without IPC_CREAT -> ENOENT");

  struct semid_ds ds;
  memset (&ds, 0, sizeof ds);
  r = semctl (id, 0, IPC_STAT, (union semun) { .buf = &ds });
  CHECK (r == 0 && ds.sem_nsems == 3 && (ds.sem_perm.mode & 0777) == 0600
         && ds.sem_perm.__key == KEY_A && ds.sem_ctime != 0,
         "IPC_STAT: nsems=%lu mode=%o key=%#x", (unsigned long) ds.sem_nsems,
         ds.sem_perm.mode & 0777, (unsigned) ds.sem_perm.__key);
  CHECK (semctl (id, 1, GETVAL) == 0, "new semaphores start at 0");
  CHECK (semctl (id, 1, SETVAL, (union semun) { .val = 7 }) == 0
         && semctl (id, 1, GETVAL) == 7, "SETVAL 7 / GETVAL -> 7");
  CHECK (semctl (id, 1, GETPID) == getpid (), "GETPID is the SETVAL caller");
  unsigned short all[3] = { 1, 2, 3 }, got[3] = { 0 };
  CHECK (semctl (id, 0, SETALL, (union semun) { .array = all }) == 0
         && semctl (id, 0, GETALL, (union semun) { .array = got }) == 0
         && got[0] == 1 && got[1] == 2 && got[2] == 3, "SETALL/GETALL round trip");
  errno = 0;
  r = semctl (id, 0, SETVAL, (union semun) { .val = 40000 });
  CHECK (r == -1 && errno == ERANGE, "SETVAL 40000 -> ERANGE");
  errno = 0;
  r = semctl (id, 5, GETVAL);
  CHECK (r == -1 && errno == EINVAL, "GETVAL of semnum 5 in a 3-set -> EINVAL");
  struct seminfo si;
  r = semctl (id, 0, IPC_INFO, (union semun) { .__buf = &si });
  CHECK (r >= 0 && si.semvmx == 32767 && si.semmsl > 0, "IPC_INFO: semmsl=%d semvmx=%d",
         si.semmsl, si.semvmx);

  /* semop basics.  */
  semctl (id, 0, SETVAL, (union semun) { .val = 0 });
  struct sembuf dec = { 0, -1, IPC_NOWAIT };
  errno = 0;
  r = semop (id, &dec, 1);
  CHECK (r == -1 && errno == EAGAIN, "semop -1 IPC_NOWAIT on 0 -> EAGAIN");
  /* The Steam pattern: semtimedop(id, {0,-1,IPC_NOWAIT}, 1, NULL) polling.  */
  int eagain = 0;
  for (int i = 0; i < 2000; i++)
    if (semtimedop (id, &dec, 1, NULL) == -1 && errno == EAGAIN)
      eagain++;
  CHECK (eagain == 2000, "2000 x semtimedop(-1, IPC_NOWAIT, NULL) polling -> EAGAIN");
  struct sembuf inc = { 0, +2, 0 };
  CHECK (semop (id, &inc, 1) == 0 && semctl (id, 0, GETVAL) == 2, "semop +2 -> 2");
  CHECK (semop (id, &dec, 1) == 0 && semctl (id, 0, GETVAL) == 1, "semop -1 -> 1");
  ds.sem_otime = 0;
  semctl (id, 0, IPC_STAT, (union semun) { .buf = &ds });
  CHECK (ds.sem_otime != 0, "sem_otime set by semop");

  /* Atomicity: {0:-1, 2:-4} with sem2 = 3 must not change sem0.  */
  semctl (id, 2, SETVAL, (union semun) { .val = 3 });
  struct sembuf two[2] = { { 0, -1, IPC_NOWAIT }, { 2, -4, IPC_NOWAIT } };
  errno = 0;
  r = semop (id, two, 2);
  CHECK (r == -1 && errno == EAGAIN && semctl (id, 0, GETVAL) == 1
         && semctl (id, 2, GETVAL) == 3, "multi-op semop is all-or-nothing");
  two[1].sem_op = -3;
  CHECK (semop (id, two, 2) == 0 && semctl (id, 0, GETVAL) == 0
         && semctl (id, 2, GETVAL) == 0, "multi-op semop applies all operations");

  struct sembuf bad = { 3, 1, 0 };
  errno = 0;
  r = semop (id, &bad, 1);
  CHECK (r == -1 && errno == EFBIG, "sem_num >= nsems -> EFBIG");
  struct sembuf big = { 0, 32767, 0 };
  semop (id, &big, 1);
  big.sem_op = 1;
  errno = 0;
  r = semop (id, &big, 1);
  CHECK (r == -1 && errno == ERANGE, "value above SEMVMX -> ERANGE");

  /* Wait-for-zero with IPC_NOWAIT, and a timed wait.  */
  struct sembuf zero = { 0, 0, IPC_NOWAIT };
  errno = 0;
  r = semop (id, &zero, 1);
  CHECK (r == -1 && errno == EAGAIN, "wait-for-zero IPC_NOWAIT on nonzero -> EAGAIN");
  semctl (id, 0, SETVAL, (union semun) { .val = 0 });
  CHECK (semop (id, &zero, 1) == 0, "wait-for-zero on 0 succeeds immediately");

  struct sembuf wdec = { 0, -1, 0 };
  struct timespec to = { 0, 300 * 1000000L };
  double t0 = now_s ();
  errno = 0;
  r = semtimedop (id, &wdec, 1, &to);
  double dt = now_s () - t0;
  CHECK (r == -1 && errno == EAGAIN && dt >= 0.28 && dt < 5,
         "semtimedop 300 ms timeout -> EAGAIN after %.0f ms", dt * 1000);
  CHECK (semctl (id, 0, GETNCNT) == 0, "GETNCNT back to 0 after the timeout");
  struct timespec badto = { 0, 1000000000L };
  errno = 0;
  r = semtimedop (id, &wdec, 1, &badto);
  CHECK (r == -1 && errno == EINVAL, "semtimedop with tv_nsec=1e9 -> EINVAL");

  CHECK (semctl (id, 0, IPC_RMID) == 0, "IPC_RMID");
  errno = 0;
  r = semop (id, &dec, 1);
  CHECK (r == -1 && errno == EINVAL, "semop on a removed id -> EINVAL");
  errno = 0;
  r = semctl (id, 0, GETVAL);
  CHECK (r == -1 && errno == EINVAL, "semctl on a removed id -> EINVAL");
  CHECK (semget (KEY_A, 1, 0600) == -1 && errno == ENOENT, "key released by IPC_RMID");

  int p = semget (IPC_PRIVATE, 1, 0600);
  int p2 = semget (IPC_PRIVATE, 1, 0600);
  CHECK (p >= 0 && p2 >= 0 && p != p2, "IPC_PRIVATE always creates a new set");
  semctl (p, 0, IPC_RMID);
  semctl (p2, 0, IPC_RMID);
}

static void
test_unrelated_wakeup (void)
{
  drop_key (KEY_B);
  int id = semget (KEY_B, 1, IPC_CREAT | IPC_EXCL | 0600);
  pid_t pid = spawn_self ("post", "B", NULL);
  struct sembuf dec = { 0, -1, 0 };
  double t0 = now_s ();
  int r = semop (id, &dec, 1);    /* blocks until the other process posts */
  double dt = now_s () - t0;
  int st = wait_status (pid);
  CHECK (r == 0 && st == 0 && semctl (id, 0, GETVAL) == 0,
         "blocking semop woken by +1 from an unrelated process (%.0f ms, helper %d)",
         dt * 1000, st);
  semctl (id, 0, IPC_RMID);
}

static void
test_rmid_wakes (void)
{
  int id = semget (IPC_PRIVATE, 1, 0600);
  pid_t pid = fork ();
  if (pid == 0)
    {
      struct sembuf dec = { 0, -1, 0 };
      int r = semop (id, &dec, 1);
      _exit (r == -1 && errno == EIDRM ? 0 : 1);
    }
  int ok = wait_for (id, GETNCNT, 1);
  CHECK (ok, "forked child sleeps in semop (GETNCNT = 1)");
  semctl (id, 0, IPC_RMID);
  CHECK (wait_status (pid) == 0, "IPC_RMID wakes the sleeper with EIDRM");

  /* Wait-for-zero sleeper woken by SETVAL 0.  */
  id = semget (IPC_PRIVATE, 1, 0600);
  semctl (id, 0, SETVAL, (union semun) { .val = 1 });
  pid = fork ();
  if (pid == 0)
    {
      struct sembuf z = { 0, 0, 0 };
      _exit (semop (id, &z, 1) == 0 ? 0 : 1);
    }
  ok = wait_for (id, GETZCNT, 1);
  semctl (id, 0, SETVAL, (union semun) { .val = 0 });
  CHECK (ok && wait_status (pid) == 0, "wait-for-zero sleeper (GETZCNT = 1) woken by SETVAL 0");
  semctl (id, 0, IPC_RMID);
}

static void
test_undo (void)
{
  drop_key (KEY_C);
  int id = semget (KEY_C, 1, IPC_CREAT | IPC_EXCL | 0600);
  semctl (id, 0, SETVAL, (union semun) { .val = 1 });
  int st = wait_status (spawn_self ("hold", "C", "exit"));
  CHECK (st == 0 && wait_for (id, GETVAL, 1),
         "SEM_UNDO: -1 undone when the holder exits (value %d)", semctl (id, 0, GETVAL));
  st = wait_status (spawn_self ("hold", "C", "kill"));
  CHECK (st == 128 + SIGKILL && wait_for (id, GETVAL, 1),
         "SEM_UNDO: -1 undone when the holder is SIGKILLed (value %d)", semctl (id, 0, GETVAL));

  /* A process's own undo balances out: -1/+1 with SEM_UNDO, then exit.  */
  pid_t pid = fork ();
  if (pid == 0)
    {
      struct sembuf a = { 0, -1, SEM_UNDO }, b = { 0, +1, SEM_UNDO };
      _exit (semop (id, &a, 1) == 0 && semop (id, &b, 1) == 0 ? 0 : 1);
    }
  st = wait_status (pid);
  sleep_ms (200);
  CHECK (st == 0 && semctl (id, 0, GETVAL) == 1, "balanced SEM_UNDO ops leave the value alone");

  /* SETVAL clears pending adjustments: holder takes it, parent SETVALs 5,
     holder exits -> value stays 5.  */
  int up[2], down[2];   /* child -> parent, parent -> child */
  if (pipe (up) != 0 || pipe (down) != 0)
    abort ();
  pid = fork ();
  if (pid == 0)
    {
      struct sembuf a = { 0, -1, SEM_UNDO };
      int r = semop (id, &a, 1);
      char c;
      if (write (up[1], "x", 1) != 1 || read (down[0], &c, 1) != 1)
        _exit (3);            /* ... waited for the parent's SETVAL */
      _exit (r == 0 ? 0 : 1);
    }
  char c;
  if (read (up[0], &c, 1) != 1)
    abort ();
  semctl (id, 0, SETVAL, (union semun) { .val = 5 });
  if (write (down[1], "y", 1) != 1)
    abort ();
  st = wait_status (pid);
  sleep_ms (200);
  CHECK (st == 0 && semctl (id, 0, GETVAL) == 5, "SETVAL clears SEM_UNDO adjustments (value %d)",
         semctl (id, 0, GETVAL));
  close (up[0]);
  close (up[1]);
  close (down[0]);
  close (down[1]);
  semctl (id, 0, IPC_RMID);
}

/* N processes use a set as a mutex (-1/+1 with SEM_UNDO) around a
   non-atomic increment in shared memory.  */
static void
test_mutex_stress (void)
{
  enum { NPROC = 4, ITERS = 400 };
  int id = semget (IPC_PRIVATE, 1, 0600);
  semctl (id, 0, SETVAL, (union semun) { .val = 1 });
  volatile int *counter = mmap (NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  *counter = 0;
  pid_t pids[NPROC];
  for (int n = 0; n < NPROC; n++)
    if ((pids[n] = fork ()) == 0)
      {
        struct sembuf lock = { 0, -1, SEM_UNDO }, unlock = { 0, +1, SEM_UNDO };
        for (int i = 0; i < ITERS; i++)
          {
            if (semop (id, &lock, 1) != 0)
              _exit (1);
            int v = *counter;
            if ((i & 7) == 0)
              sched_yield ();
            *counter = v + 1;
            if (semop (id, &unlock, 1) != 0)
              _exit (2);
          }
        _exit (0);
      }
  int bad = 0;
  for (int n = 0; n < NPROC; n++)
    bad |= wait_status (pids[n]) != 0;
  CHECK (!bad && *counter == NPROC * ITERS && semctl (id, 0, GETVAL) == 1,
         "mutual exclusion across %d processes: counter %d / %d", NPROC, *counter,
         NPROC * ITERS);
  semctl (id, 0, IPC_RMID);
}

/* Raw syscall(2) path (what FEX uses): routed by the glibc wrapper.  */
static void
test_syscall_path (void)
{
#ifndef SYS_semop
  /* i386 has only ipc() and semtimedop_time64: covered by the libc calls.  */
  printf ("ok - raw sem syscalls skipped (not on this architecture)\n");
#else
  long id = syscall (SYS_semget, IPC_PRIVATE, 2, IPC_CREAT | 0600);
  CHECK (id >= 0, "syscall(SYS_semget) -> id %ld", id);
  CHECK (syscall (SYS_semctl, id, 1, SETVAL, 5L) == 0
         && syscall (SYS_semctl, id, 1, GETVAL, 0L) == 5,
         "syscall(SYS_semctl SETVAL 5 (value passed directly) / GETVAL)");
#ifdef __aarch64__
  /* fexdroid extension: IPC_64 in cmd is ignored (FEX's i386 paths may pass
     it through).  Real 64-bit kernels, and FEX's x86-64 handler, reject it.  */
  CHECK (syscall (SYS_semctl, id, 1, 0x100 | SETVAL, 6L) == 0
         && semctl (id, 1, GETVAL) == 6, "syscall(SYS_semctl IPC_64|SETVAL) (i386 ipc style)");
#else
  CHECK (syscall (SYS_semctl, id, 1, SETVAL, 6L) == 0 && semctl (id, 1, GETVAL) == 6,
         "syscall(SYS_semctl SETVAL 6)");
#endif
  struct sembuf sb = { 1, -4, 0 };
  struct timespec to = { 1, 0 };
  CHECK (syscall (SYS_semtimedop, id, &sb, 1L, &to) == 0 && semctl (id, 1, GETVAL) == 2,
         "syscall(SYS_semtimedop -4) -> 2");
  sb.sem_flg = IPC_NOWAIT;
  errno = 0;
  long r = syscall (SYS_semop, id, &sb, 1L);
  CHECK (r == -1 && errno == EAGAIN, "syscall(SYS_semop -4, IPC_NOWAIT) on 2 -> EAGAIN");
  CHECK (syscall (SYS_semctl, id, 0, IPC_RMID, 0L) == 0, "syscall(SYS_semctl IPC_RMID)");
#endif
}

static void
test_msg_enosys (void)
{
  errno = 0;
  int r = msgget (IPC_PRIVATE, IPC_CREAT | 0600);
  CHECK (r == -1 && errno == ENOSYS, "msgget still -> ENOSYS (no syscall issued)");
  if (r >= 0)
    msgctl (r, IPC_RMID, NULL);   /* running natively on a real kernel */
}

int
main (int argc, char **argv)
{
  self = argv[0];
  setvbuf (stdout, NULL, _IOLBF, 0);
  key_t keys[3] = { KEY_A, KEY_B, KEY_C };
  if (argc >= 3 && strcmp (argv[1], "post") == 0)
    return do_post (keys[argv[2][0] - 'A']);
  if (argc >= 4 && strcmp (argv[1], "hold") == 0)
    return do_hold (keys[argv[2][0] - 'A'], argv[3]);
  if (argc >= 2 && strcmp (argv[1], "all") == 0)
    {
      test_basic ();
      test_unrelated_wakeup ();
      test_rmid_wakes ();
      test_undo ();
      test_mutex_stress ();
      test_syscall_path ();
      test_msg_enosys ();
      printf ("semtest: %s\n", failures ? "FAILURES" : "all ok");
      return failures != 0;
    }
  fprintf (stderr, "usage: %s all\n", argv[0]);
  return 2;
}
