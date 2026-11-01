// Checks glibc's Android seccomp workarounds (patches/glibc/0003): trapped
// syscalls must fail with ENOSYS (or be routed) instead of raising SIGSYS.
// On the PC under qemu nothing is trapped, so this verifies the routing logic;
// on the phone it proves the process survives.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;
static void expect(const char *what, long r, int want_errno) {
    int e = errno;
    int ok = want_errno ? (r == -1 && e == want_errno) : (r >= 0);
    printf("%-40s r=%ld errno=%s -> %s\n", what, r, r == -1 ? strerror(e) : "-", ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    expect("syscall(set_robust_list) -> 0", syscall(SYS_set_robust_list, NULL, 24), 0);
    expect("syscall(rseq) -> ENOSYS", syscall(SYS_rseq, NULL, 0, 0, 0), ENOSYS);
    expect("syscall(faccessat2) -> ENOSYS", syscall(SYS_faccessat2, AT_FDCWD, "/", F_OK, 0), ENOSYS);
    long sid = syscall(SYS_semget, IPC_PRIVATE, 1, 0600);
    expect("syscall(semget) routed to fxsem", sid, 0);
    if (sid >= 0) expect("syscall(semctl IPC_RMID) routed", syscall(SYS_semctl, sid, 0, IPC_RMID, 0L), 0);
    expect("syscall(msgget) -> ENOSYS", syscall(SYS_msgget, IPC_PRIVATE, 0600), ENOSYS);
    expect("syscall(openat2) -> ENOSYS", syscall(SYS_openat2, AT_FDCWD, "/", NULL, 0), ENOSYS);
    long id = syscall(SYS_shmget, IPC_PRIVATE, 4096, 0600);
    expect("syscall(shmget) routed to fxshm", id, 0);
    if (id >= 0) expect("syscall(shmctl IPC_RMID) routed", syscall(SYS_shmctl, id, IPC_RMID, NULL), 0);
    expect("setuid(getuid()) -> 0 (no-op)", setuid(getuid()), 0);
    expect("setgid(getgid()) -> 0 (no-op)", setgid(getgid()), 0);
    expect("syscall(mount) -> ENOSYS", syscall(SYS_mount, "a", "b", "c", 0, NULL), ENOSYS);
    expect("access(\"/\") (faccessat fallback)", access("/", F_OK), 0);
    expect("getpid() still works", getpid(), 0);
    printf("seccomp-wrap: %s\n", failures ? "FAILURES" : "all ok");
    return failures != 0;
}
