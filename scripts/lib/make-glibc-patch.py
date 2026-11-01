#!/usr/bin/env python3
"""Regenerates patches/glibc/0001-*.patch from exact-match edits.

Kept as a script (not just the .patch) so the intent of every hunk is readable
and the patch can be re-derived when bumping the glibc version.
"""
import difflib, pathlib, sys

src = pathlib.Path(sys.argv[1])        # pristine glibc source tree
out = pathlib.Path(sys.argv[2])        # patch file to write

NOTE_ROBUST = ("/* fexdroid: Android's app seccomp filter traps set_robust_list with\n"
               "{i}   SIGSYS, so it is never called.  Robust mutexes still work in\n"
               "{i}   userspace; only the kernel's cleanup on thread death is lost.  */")

EDITS = {
    "sysdeps/nptl/dl-tls_init_tp.c": [
        ("""    int res = INTERNAL_SYSCALL_CALL (set_robust_list, &pd->robust_head,
                                     sizeof (struct robust_list_head));
""",
         "    " + NOTE_ROBUST.format(i="    ") + "\n    int res = 0;\n"),
        ("""    bool do_rseq = TUNABLE_GET (rseq, int, NULL);""",
         """    /* fexdroid: rseq is trapped by Android's app seccomp filter.  */
    bool do_rseq = false;"""),
    ],
    "nptl/pthread_create.c": [
        ("""      /* This call should never fail because the initial call in init.c
	 succeeded.  */
      INTERNAL_SYSCALL_CALL (set_robust_list, &pd->robust_head,
			     sizeof (struct robust_list_head));
""",
         "      " + NOTE_ROBUST.format(i="      ") + "\n"),
    ],
    "sysdeps/nptl/_Fork.c": [
        ("""      INTERNAL_SYSCALL_CALL (set_robust_list, &self->robust_head,
			     sizeof (struct robust_list_head));
""",
         "      " + NOTE_ROBUST.format(i="      ") + "\n"),
    ],
    "sysdeps/unix/sysv/linux/faccessat.c": [
        ("""  int ret = INLINE_SYSCALL_CALL (faccessat2, fd, file, mode, flag);
#if __ASSUME_FACCESSAT2
  return ret;
#else
  if (ret == 0 || errno != ENOSYS)
    return ret;
""",
         """#if __ASSUME_FACCESSAT2
  return INLINE_SYSCALL_CALL (faccessat2, fd, file, mode, flag);
#else
  /* fexdroid: faccessat2 is trapped by Android's app seccomp filter
     (SIGSYS, not ENOSYS), so go straight to the faccessat fallback.  */
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
out.write_text("".join(chunks))
print(f"wrote {out}")
