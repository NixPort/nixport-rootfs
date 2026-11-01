#!/usr/bin/env python3
"""Regenerates patches/glibc/0003-android-syscall-wrapper.patch.

Makes the generic syscall(2) function safe under Android's app seccomp filter:
  * aarch64/syscall.S now defines the hidden __fxd_raw_syscall instead of syscall;
  * a new C syscall() checks the number against the AOSP-derived table
    (scripts/lib/gen-android-seccomp.py) and, for trapped numbers, either routes
    to a safe libc implementation (SysV shm and semaphores over fxshmd, accept -> accept4,
    set_robust_list -> success without kernel registration) or fails with ENOSYS.
Programs that issue raw syscalls through syscall() (FEX's host side, Chromium/CEF,
...) then get an error instead of SIGSYS.  Inline `svc` sites are not covered.

usage: make-glibc-syscall-patch.py <pristine glibc src> <aosp-dir> <unistd.h> <patch out>
"""
import difflib, pathlib, subprocess, sys

src, aosp, unistd, out = (pathlib.Path(a) for a in sys.argv[1:5])
here = pathlib.Path(__file__).resolve().parent
A64 = "sysdeps/unix/sysv/linux/aarch64/"

table = subprocess.run([sys.executable, str(here / "gen-android-seccomp.py"), str(aosp), str(unistd), "c"],
                       check=True, capture_output=True, text=True).stdout

WRAPPER = r'''/* fexdroid: syscall(2) that survives Android's app seccomp filter.
   See scripts/lib/make-glibc-syscall-patch.py and NOTES.md N-018.  */
#include <errno.h>
#include <stdarg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sysdep.h>
#include <fxshm.h>
#include "fxd-android-seccomp.h"

extern long int __fxd_raw_syscall (long int number, ...);

long int
syscall (long int number, ...)
{
  va_list args;
  va_start (args, number);
  long int a0 = va_arg (args, long int);
  long int a1 = va_arg (args, long int);
  long int a2 = va_arg (args, long int);
  long int a3 = va_arg (args, long int);
  long int a4 = va_arg (args, long int);
  long int a5 = va_arg (args, long int);
  va_end (args);

  if (__glibc_unlikely (fxd_android_trapped (number)))
    switch (number)
      {
      case __NR_shmget:
	return shmget ((key_t) a0, (size_t) a1, (int) a2);
      case __NR_shmctl:
	return shmctl ((int) a0, (int) a1, (struct shmid_ds *) a2);
      case __NR_shmat:
	return (long int) shmat ((int) a0, (const void *) a1, (int) a2);
      case __NR_shmdt:
	return shmdt ((const void *) a0);
      /* SysV semaphores (fxsem-client.c).  The raw kernel ABI is kept:
	 semctl's 4th argument is the union semun value itself.  */
      case __NR_semget:
	return __fxsem_get ((key_t) a0, (int) a1, (int) a2);
      case __NR_semop:
	return __fxsem_timedop ((int) a0, (struct sembuf *) a1, (size_t) a2,
				NULL);
      case __NR_semtimedop:
	return __fxsem_timedop ((int) a0, (struct sembuf *) a1, (size_t) a2,
				(const struct timespec *) a3);
      case __NR_semctl:
	return __fxsem_ctl ((int) a0, (int) a1, (int) a2,
			    (unsigned long int) a3);
      case __NR_accept:
	return accept4 ((int) a0, (struct sockaddr *) a1, (socklen_t *) a2, 0);
      case __NR_set_robust_list:
	/* Accept the registration; only the kernel's cleanup of robust
	   mutexes held by a dying thread is lost (see patch 0001).  */
	return 0;
      default:
	if (fxd_android_fake_success (number))
	  return 0;
	__set_errno (ENOSYS);
	return -1;
      }

  return __fxd_raw_syscall (number, a0, a1, a2, a3, a4, a5);
}
'''

# Rename the assembly entry point; the C wrapper below becomes syscall().
# (Not exported: it is absent from the Versions files, so the libc version
# script keeps it local.)
rel = A64 + "syscall.S"
text = (src / rel).read_text().replace("ENTRY (syscall)", "ENTRY (__fxd_raw_syscall)") \
                               .replace("PSEUDO_END (syscall)", "PSEUDO_END (__fxd_raw_syscall)")
files = [(rel, (src / rel).read_text(), text)]

# sysdep.h: every constant-number syscall site (C INTERNAL_SYSCALL and the
# assembler DO_CALL used by the generated wrappers) returns -ENOSYS at compile
# time when Android traps that number, instead of executing svc.
sd = A64 + "sysdep.h"
sd_old = (src / sd).read_text()
sd_new = sd_old
for before, after in [
    ("#include <tls.h>\n",
     "#include <tls.h>\n\n/* fexdroid: syscalls trapped by Android's app seccomp filter.  */\n"
     "#include \"fxd-android-seccomp.h\"\n"),
    ("    mov x8, SYS_ify (syscall_name);\t\t\\\n    svc 0\n",
     "    FXD_SVC SYS_ify (syscall_name)\n"),
    ("""  ({ long _sys_result;\t\t\t\t\t\t\\
     {\t\t\t\t\t\t\t\t\\
       LOAD_ARGS_##nr (args)\t\t\t\t\t\\""",
     """  ({ long _sys_result;\t\t\t\t\t\t\\
     if (__builtin_constant_p (name) && fxd_android_trapped (name)) \\
       _sys_result = fxd_android_fake_success (name) ? 0 : -38; /* fexdroid */ \\
     else\t\t\t\t\t\t\t\\
     {\t\t\t\t\t\t\t\t\\
       LOAD_ARGS_##nr (args)\t\t\t\t\t\\"""),
]:
    if sd_new.count(before) != 1:
        sys.exit(f"sysdep.h: expected one match for {before!r}")
    sd_new = sd_new.replace(before, after)
files.append((sd, sd_old, sd_new))

# clone3.S issues its syscall explicitly (not via DO_CALL). clone3 is trapped
# before Android 15; with ENOSYS glibc falls back to clone (clone-internal.c).
c3 = A64 + "clone3.S"
c3_old = (src / c3).read_text()
before = "\tmov\tx8, #SYS_ify(clone3)\n\tsvc\t0x0\n"
if c3_old.count(before) != 1:
    sys.exit("clone3.S: expected one explicit clone3 svc")
files.append((c3, c3_old, c3_old.replace(before, "\tFXD_SVC SYS_ify(clone3)\t/* fexdroid */\n")))

# Cancellable syscalls (read, accept, connect, ...) reach the kernel through
# __internal_syscall_cancel with the number in a register, bypassing the
# compile-time guard above. Check it at runtime there (e.g. FEXServer's accept()).
cc = "nptl/cancellation.c"
cc_old = (src / cc).read_text()
before = """			   __syscall_arg_t nr)
{
  long int result;
  struct pthread *pd = THREAD_SELF;
"""
after = """			   __syscall_arg_t nr)
{
  long int result;
  struct pthread *pd = THREAD_SELF;

  /* fexdroid: syscalls trapped by Android's app seccomp filter.  */
  if (__glibc_unlikely (fxd_android_trapped (nr)))
    {
      if (nr == __NR_accept)
	{
	  nr = __NR_accept4;	/* accept(fd, a, l) == accept4(fd, a, l, 0) */
	  a4 = 0;
	}
      else
	return fxd_android_fake_success (nr) ? 0 : -ENOSYS;
    }
"""
if cc_old.count(before) != 1:
    sys.exit("cancellation.c: anchor not found")
cc_new = cc_old.replace(before, after)
inc = '#include "pthreadP.h"\n'
if cc_new.count(inc) != 1:
    sys.exit("cancellation.c: include anchor not found")
cc_new = cc_new.replace(inc, inc + '#include <fxd-android-seccomp.h>\n')
files.append((cc, cc_old, cc_new))

mk = A64 + "Makefile"
mk_old = (src / mk).read_text()
mk_new = mk_old + "\n# fexdroid: C syscall() wrapper (see fxd-syscall.c)\nifeq ($(subdir),misc)\nsysdep_routines += fxd-syscall\nendif\n"
files.append((mk, mk_old, mk_new))
files.append((A64 + "fxd-syscall.c", "", WRAPPER))
files.append((A64 + "fxd-android-seccomp.h", "", table))

chunks = []
for rel, old, new in files:
    a = f"a/{rel}" if old else "/dev/null"
    chunks += difflib.unified_diff(old.splitlines(True), new.splitlines(True), a, f"b/{rel}")
out.write_text("".join(chunks))
print(f"wrote {out}")
