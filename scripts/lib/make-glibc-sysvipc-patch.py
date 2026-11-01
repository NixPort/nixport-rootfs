#!/usr/bin/env python3
"""Regenerates patches/glibc/0002-android-sysvipc-userspace.patch.

Companion of make-glibc-patch.py (0001), same idea: every hunk is an
exact-match edit written out here, so the intent stays readable and the patch
can be re-derived for a new glibc version.  New files come from
patches/glibc/fxshm/ and the wire protocol header from tools/fxshmd/ (single
source of truth shared with the daemon).

  usage: make-glibc-sysvipc-patch.py <pristine glibc src> <patch to write>

What the patch does (see NOTES.md N-017):
  * shmget/shmat/shmdt/shmctl -> userspace implementation over memfd + fxshmd
    (sysdeps/unix/sysv/linux/fxshm-client.c), no shm* syscalls issued.
  * semget/semop/semtimedop/semctl -> userspace implementation: each set is a
    memfd from fxshmd mapped MAP_SHARED, operations under a process-shared
    lock, sleeping on futexes (fxsem-client.c, layout in fxsem-layout.h;
    NOTES.md N-025).
  * msgget/msgsnd/msgrcv/msgctl -> fail with ENOSYS without issuing the
    (SIGSYS-trapped) syscall.
"""
import difflib, pathlib, sys

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
here = pathlib.Path(__file__).resolve().parent.parent.parent   # repo root
LINUX = "sysdeps/unix/sysv/linux/"

NEW_FILES = {
    LINUX + "fxshm-proto.h": here / "tools/fxshmd/fxshm-proto.h",
    LINUX + "fxshm-config.h": here / "patches/glibc/fxshm/fxshm-config.h",
    LINUX + "fxshm.h": here / "patches/glibc/fxshm/fxshm.h",
    LINUX + "fxshm-client.c": here / "patches/glibc/fxshm/fxshm-client.c",
    LINUX + "fxsem-layout.h": here / "tools/fxshmd/fxsem-layout.h",
    LINUX + "fxsem-client.c": here / "patches/glibc/fxshm/fxsem-client.c",
}

MSG_NOTE = ("  /* fexdroid: SysV message queues are trapped (SIGSYS) by Android's app seccomp\n"
            "     filter.  Fail without issuing the syscall.  */\n"
            "  __set_errno (ENOSYS);\n  return -1;\n")
SEM_NOTE = MSG_NOTE.replace("message queues", "semaphores")

EDITS = {
    # ---- shared memory: route to fxshm-client.c ----------------------------
    LINUX + "shmget.c": [
        ("#include <sysdep.h>\n", "#include <sysdep.h>\n#include <fxshm.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (shmget, key, size, shmflg, NULL);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_shmget, key, size, shmflg, NULL);
#endif
""",
         """  /* fexdroid: shmget is trapped (SIGSYS) by Android's app seccomp filter;
     segments are memfds handed out by fxshmd (see fxshm-client.c).  */
  return __fxshm_get (key, size, shmflg);
"""),
    ],
    LINUX + "shmat.c": [
        ("#include <errno.h>\n", "#include <errno.h>\n#include <fxshm.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return (void*) INLINE_SYSCALL_CALL (shmat, shmid, shmaddr, shmflg);
#else
  unsigned long resultvar;
  void *raddr;

  resultvar = INTERNAL_SYSCALL_CALL (ipc, IPCOP_shmat, shmid, shmflg,
				     &raddr, shmaddr);
  if (INTERNAL_SYSCALL_ERROR_P (resultvar))
    return (void *) INLINE_SYSCALL_ERROR_RETURN_VALUE (INTERNAL_SYSCALL_ERRNO (resultvar));

  return raddr;
#endif
""",
         """  /* fexdroid: shmat is trapped (SIGSYS) by Android's app seccomp filter;
     mmap the segment's memfd instead (see fxshm-client.c).  */
  return __fxshm_at (shmid, shmaddr, shmflg);
"""),
    ],
    LINUX + "shmdt.c": [
        ("#include <errno.h>\n", "#include <errno.h>\n#include <fxshm.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (shmdt, shmaddr);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_shmdt, 0, 0, 0, shmaddr);
#endif
""",
         """  /* fexdroid: shmdt is trapped (SIGSYS) by Android's app seccomp filter
     (see fxshm-client.c).  */
  return __fxshm_dt (shmaddr);
"""),
    ],
    LINUX + "shmctl.c": [
        ("#include <linux/posix_types.h>  /* For __kernel_mode_t.  */\n",
         "#include <linux/posix_types.h>  /* For __kernel_mode_t.  */\n#include <fxshm.h>\n"),
        ("""static int
shmctl_syscall (int shmid, int cmd, shmctl_arg_t *buf)
{
#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (shmctl, shmid, cmd | __IPC_64, buf);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_shmctl, shmid, cmd | __IPC_64, 0,
			      buf);
#endif
}
""",
         """static int
shmctl_syscall (int shmid, int cmd, shmctl_arg_t *buf)
{
  /* fexdroid: shmctl is trapped (SIGSYS) by Android's app seccomp filter;
     ask fxshmd instead (see fxshm-client.c).  Only the 64-bit layout, where
     shmctl_arg_t is struct shmid_ds, is supported.  */
#if __IPC_TIME64
# error "fexdroid fxshm: 32-bit time64 shmid_ds translation not implemented"
#endif
  return __fxshm_ctl (shmid, cmd & ~__IPC_64, (struct shmid_ds *) buf);
}
"""),
    ],
    # ---- semaphores: route to fxsem-client.c -------------------------------
    LINUX + "semget.c": [
        ("#include <errno.h>\n", "#include <errno.h>\n#include <fxshm.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (semget, key, nsems, semflg);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_semget, key, nsems, semflg, NULL);
#endif
""", """  /* fexdroid: semget is trapped (SIGSYS) by Android's app seccomp filter;
     semaphore sets are memfds handed out by fxshmd (see fxsem-client.c).  */
  return __fxsem_get (key, nsems, semflg);
"""),
    ],
    LINUX + "semtimedop.c": [
        ("#include <errno.h>\n", "#include <errno.h>\n#include <fxshm.h>\n"),
        ("""#ifdef __NR_semtimedop_time64
  return INLINE_SYSCALL_CALL (semtimedop_time64, semid, sops, nsops, timeout);
#elif defined __ASSUME_DIRECT_SYSVIPC_SYSCALLS && defined __NR_semtimedop
  return INLINE_SYSCALL_CALL (semtimedop, semid, sops, nsops, timeout);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_semtimedop, semid,
			      SEMTIMEDOP_IPC_ARGS (nsops, sops, timeout));
#endif
""", """  /* fexdroid: semtimedop is trapped (SIGSYS) by Android's app seccomp
     filter; operate on the set's shared mapping (see fxsem-client.c).
     64-bit time_t only (aarch64): __timespec64 is struct timespec.  */
#if __TIMESIZE != 64
# error "fexdroid fxsem: 32-bit time_t not implemented"
#endif
  return __fxsem_timedop (semid, sops, nsops,
			  (const struct timespec *) timeout);
"""),
        # 32-bit time_t fallback path (not compiled on aarch64, kept consistent).
        ("""# ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (semtimedop, semid, sops, nsops, pts32);
# else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_semtimedop, semid,
			      SEMTIMEDOP_IPC_ARGS (nsops, sops, pts32));
# endif
""", SEM_NOTE),
    ],
    LINUX + "semctl.c": [
        ("#include <linux/posix_types.h>             /* For __kernel_mode_t.  */\n",
         "#include <linux/posix_types.h>             /* For __kernel_mode_t.  */\n#include <fxshm.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (semctl, semid, semnum, cmd | __IPC_64,
			      arg.array);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_semctl, semid, semnum, cmd | __IPC_64,
			      SEMCTL_ARG_ADDRESS (arg));
#endif
""", """  /* fexdroid: semctl is trapped (SIGSYS) by Android's app seccomp filter;
     work on the set's shared mapping / ask fxshmd (see fxsem-client.c).
     Only the 64-bit layout (semctl_arg_t is union semun) is supported.  */
#if __IPC_TIME64
# error "fexdroid fxsem: 32-bit time64 semid_ds translation not implemented"
#endif
  return __fxsem_ctl (semid, semnum, cmd, (unsigned long int) arg.array);
"""),
    ],
    # ---- message queues: ENOSYS --------------------------------------------
    LINUX + "msgget.c": [
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (msgget, key, msgflg);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_msgget, key, msgflg, 0, NULL);
#endif
""", MSG_NOTE),
    ],
    LINUX + "msgsnd.c": [
        ("#include <sysdep-cancel.h>\n", "#include <sysdep-cancel.h>\n#include <errno.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return SYSCALL_CANCEL (msgsnd, msqid, msgp, msgsz, msgflg);
#else
  return SYSCALL_CANCEL (ipc, IPCOP_msgsnd, msqid, msgsz, msgflg,
			 msgp);
#endif
""", MSG_NOTE),
    ],
    LINUX + "msgrcv.c": [
        ("#include <sysdep-cancel.h>\n", "#include <sysdep-cancel.h>\n#include <errno.h>\n"),
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return SYSCALL_CANCEL (msgrcv, msqid, msgp, msgsz, msgtyp, msgflg);
#else
  return SYSCALL_CANCEL (ipc, IPCOP_msgrcv, msqid, msgsz, msgflg,
			 MSGRCV_ARGS (msgp, msgtyp));
#endif
""", MSG_NOTE),
    ],
    LINUX + "msgctl.c": [
        ("""#ifdef __ASSUME_DIRECT_SYSVIPC_SYSCALLS
  return INLINE_SYSCALL_CALL (msgctl, msqid, cmd | __IPC_64, buf);
#else
  return INLINE_SYSCALL_CALL (ipc, IPCOP_msgctl, msqid, cmd | __IPC_64, 0,
			      buf);
#endif
""", MSG_NOTE),
    ],
    # ---- build the client into libc (sysvipc subdir) -----------------------
    LINUX + "Makefile": [
        ("""ifeq ($(subdir),csu)
sysdep_routines += \\
  errno-loc \\
  # sysdep_routines
endif
""",
         """ifeq ($(subdir),csu)
sysdep_routines += \\
  errno-loc \\
  # sysdep_routines
endif

# fexdroid: userspace SysV shared memory and semaphores (memfd + fxshmd),
# see fxshm-client.c and fxsem-client.c.
ifeq ($(subdir),sysvipc)
sysdep_routines += \\
  fxsem-client \\
  fxshm-client \\
  # sysdep_routines
endif
"""),
    ],
}

chunks = []
for rel, edits in EDITS.items():
    old = (src / rel).read_text()
    new = old
    for before, after in edits:
        if new.count(before) != 1:
            sys.exit(f"{rel}: expected exactly one match for:\n{before}")
        new = new.replace(before, after)
    chunks += difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                   f"a/{rel}", f"b/{rel}")
for rel, path in NEW_FILES.items():
    if (src / rel).exists():
        sys.exit(f"{rel}: already exists in the source tree")
    chunks += difflib.unified_diff([], path.read_text().splitlines(True),
                                   "/dev/null", f"b/{rel}")
out.write_text("".join(chunks))
print(f"wrote {out}")
