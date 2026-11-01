// fxpath: LD_PRELOAD shim that maps absolute Linux paths into the fexdroid rootfs.
//
// Android has no /tmp, /usr, /var, ...; Debian binaries (Xvfb, xkbcomp,
// PulseAudio) hardcode them. For the prefixes below, a path P becomes
// $FXD_ROOT/P. /dev, /proc, /sys, /data, /system etc. pass through unchanged.
// glibc itself was built with these paths inside the rootfs; this covers the
// programs' own hardcoded strings. x86 guests don't need it: FEX overlays
// their RootFS.
//
// Only libc entry points that take paths are wrapped; raw syscalls are not.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utime.h>

// "/dev/shm": Android has none; programs that share memory through files there (Valve's
// guest UIs and their web helpers) name it outright. The rest of /dev is the phone's.
static const char *const kPrefixes[] = { "/tmp", "/usr", "/etc", "/var", "/bin", "/sbin",
                                         "/lib", "/opt", "/run", "/root", "/home", "/srv", "/dev/shm" };
static char g_root[PATH_MAX];
static size_t g_root_len;
static int g_debug;  // FXPATH_DEBUG=1: log every mapping to stderr

__attribute__((constructor)) static void fxpath_init(void) {
    const char *r = getenv("FXD_ROOT");
    if (r && *r && strlen(r) < sizeof g_root - 1) {
        strcpy(g_root, r);
        g_root_len = strlen(g_root);
    }
    const char *d = getenv("FXPATH_DEBUG");
    g_debug = d && *d == '1';
}

// Returns the path to use: either `path` itself or `buf` holding the mapped path.
static const char *map(const char *path, char *buf) {
    if (!path || path[0] != '/' || !g_root_len) return path;
    if (strncmp(path, g_root, g_root_len) == 0) return path;  // already inside
    for (size_t i = 0; i < sizeof kPrefixes / sizeof *kPrefixes; i++) {
        size_t n = strlen(kPrefixes[i]);
        if (strncmp(path, kPrefixes[i], n) == 0 && (path[n] == '/' || path[n] == '\0')) {
            if (g_root_len + strlen(path) + 1 > PATH_MAX) return path;
            memcpy(buf, g_root, g_root_len);
            strcpy(buf + g_root_len, path);
            if (g_debug) dprintf(2, "fxpath[%d]: %s -> %s\n", getpid(), path, buf);
            return buf;
        }
    }
    return path;
}

#define REAL(ret, name, ...) \
    static ret (*real_##name)(__VA_ARGS__); \
    if (!real_##name) real_##name = (ret (*)(__VA_ARGS__))dlsym(RTLD_NEXT, #name)

// ---- open family (mode only matters with O_CREAT/O_TMPFILE) --------------------
#define OPEN_MODE(flags, mode) \
    mode_t mode = 0; \
    if ((flags) & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }

int open(const char *path, int flags, ...) {
    OPEN_MODE(flags, mode);
    REAL(int, open, const char *, int, ...);
    char b[PATH_MAX];
    return real_open(map(path, b), flags, mode);
}
int open64(const char *path, int flags, ...) {
    OPEN_MODE(flags, mode);
    REAL(int, open64, const char *, int, ...);
    char b[PATH_MAX];
    return real_open64(map(path, b), flags, mode);
}
int openat(int dirfd, const char *path, int flags, ...) {
    OPEN_MODE(flags, mode);
    REAL(int, openat, int, const char *, int, ...);
    char b[PATH_MAX];
    return real_openat(dirfd, map(path, b), flags, mode);
}
int openat64(int dirfd, const char *path, int flags, ...) {
    OPEN_MODE(flags, mode);
    REAL(int, openat64, int, const char *, int, ...);
    char b[PATH_MAX];
    return real_openat64(dirfd, map(path, b), flags, mode);
}
FILE *fopen(const char *path, const char *m) {
    REAL(FILE *, fopen, const char *, const char *);
    char b[PATH_MAX];
    return real_fopen(map(path, b), m);
}
FILE *fopen64(const char *path, const char *m) {
    REAL(FILE *, fopen64, const char *, const char *);
    char b[PATH_MAX];
    return real_fopen64(map(path, b), m);
}

// ---- one-path functions ------------------------------------------------------------
#define WRAP1(ret, name, T1)                                     \
    ret name(const char *p, T1 a) {                              \
        REAL(ret, name, const char *, T1);                       \
        char b[PATH_MAX];                                        \
        return real_##name(map(p, b), a);                        \
    }
WRAP1(int, access, int)
WRAP1(int, mkdir, mode_t)
WRAP1(int, chmod, mode_t)
WRAP1(int, stat, struct stat *)
WRAP1(int, lstat, struct stat *)
WRAP1(int, stat64, struct stat64 *)
WRAP1(int, lstat64, struct stat64 *)
WRAP1(int, truncate, off_t)

int unlink(const char *p) { REAL(int, unlink, const char *); char b[PATH_MAX]; return real_unlink(map(p, b)); }
int rmdir(const char *p) { REAL(int, rmdir, const char *); char b[PATH_MAX]; return real_rmdir(map(p, b)); }
int chdir(const char *p) { REAL(int, chdir, const char *); char b[PATH_MAX]; return real_chdir(map(p, b)); }
ssize_t readlink(const char *p, char *buf, size_t n) {
    REAL(ssize_t, readlink, const char *, char *, size_t);
    char b[PATH_MAX];
    return real_readlink(map(p, b), buf, n);
}
int chown(const char *p, uid_t u, gid_t g) {
    REAL(int, chown, const char *, uid_t, gid_t);
    char b[PATH_MAX];
    return real_chown(map(p, b), u, g);
}

// ---- what programs built against an older glibc call --------------------------------
// Until glibc 2.33 stat() was an inline that called __xstat(version, ...); programs built
// then (native arm64 guest UIs) still ask for those names.
// glibc keeps the old names only for such programs (dlsym does not find them), so they
// are answered with today's functions; on arm64 the structures are the same.
int __xstat(int v, const char *p, struct stat *st) { (void)v; return stat(p, st); }
int __lxstat(int v, const char *p, struct stat *st) { (void)v; return lstat(p, st); }
int __xstat64(int v, const char *p, struct stat64 *st) { (void)v; return stat64(p, st); }
int __lxstat64(int v, const char *p, struct stat64 *st) { (void)v; return lstat64(p, st); }
int __fxstatat(int v, int d, const char *p, struct stat *st, int f) { (void)v; return fstatat(d, p, st, f); }
int __fxstatat64(int v, int d, const char *p, struct stat64 *st, int f) { (void)v; return fstatat64(d, p, st, f); }

// ---- more one-path functions -----------------------------------------------------------
#define WRAP0(ret, name)                                         \
    ret name(const char *p) {                                    \
        REAL(ret, name, const char *);                           \
        char b[PATH_MAX];                                        \
        return real_##name(map(p, b));                           \
    }
WRAP0(DIR *, opendir)
WRAP0(int, remove)
WRAP1(int, creat, mode_t)
WRAP1(int, creat64, mode_t)
WRAP1(int, euidaccess, int)
WRAP1(int, eaccess, int)
WRAP1(int, mkfifo, mode_t)
WRAP1(int, truncate64, off64_t)
WRAP1(int, statfs, struct statfs *)
WRAP1(int, statfs64, struct statfs64 *)
WRAP1(int, statvfs, struct statvfs *)
WRAP1(int, statvfs64, struct statvfs64 *)
WRAP1(int, utime, const struct utimbuf *)
WRAP1(int, utimes, const struct timeval *)
WRAP1(long, pathconf, int)
int lchown(const char *p, uid_t u, gid_t g) {
    REAL(int, lchown, const char *, uid_t, gid_t);
    char b[PATH_MAX];
    return real_lchown(map(p, b), u, g);
}
FILE *freopen(const char *path, const char *m, FILE *f) {
    REAL(FILE *, freopen, const char *, const char *, FILE *);
    char b[PATH_MAX];
    return real_freopen(map(path, b), m, f);
}
FILE *freopen64(const char *path, const char *m, FILE *f) {
    REAL(FILE *, freopen64, const char *, const char *, FILE *);
    char b[PATH_MAX];
    return real_freopen64(map(path, b), m, f);
}
int scandir(const char *p, struct dirent ***l, int (*sel)(const struct dirent *),
            int (*cmp)(const struct dirent **, const struct dirent **)) {
    REAL(int, scandir, const char *, struct dirent ***, int (*)(const struct dirent *),
         int (*)(const struct dirent **, const struct dirent **));
    char b[PATH_MAX];
    return real_scandir(map(p, b), l, sel, cmp);
}
int scandir64(const char *p, struct dirent64 ***l, int (*sel)(const struct dirent64 *),
              int (*cmp)(const struct dirent64 **, const struct dirent64 **)) {
    REAL(int, scandir64, const char *, struct dirent64 ***, int (*)(const struct dirent64 *),
         int (*)(const struct dirent64 **, const struct dirent64 **));
    char b[PATH_MAX];
    return real_scandir64(map(p, b), l, sel, cmp);
}
int utimensat(int d, const char *p, const struct timespec t[2], int f) {
    REAL(int, utimensat, int, const char *, const struct timespec *, int);
    char b[PATH_MAX];
    return real_utimensat(d, map(p, b), t, f);
}
int fchmodat(int d, const char *p, mode_t m, int f) {
    REAL(int, fchmodat, int, const char *, mode_t, int);
    char b[PATH_MAX];
    return real_fchmodat(d, map(p, b), m, f);
}
int inotify_add_watch(int fd, const char *p, uint32_t mask) {
    REAL(int, inotify_add_watch, int, const char *, uint32_t);
    char b[PATH_MAX];
    return real_inotify_add_watch(fd, map(p, b), mask);
}
// A library named with its whole path; names without "/" go through ld.so's own search.
void *dlopen(const char *p, int flags) {
    REAL(void *, dlopen, const char *, int);
    char b[PATH_MAX];
    return real_dlopen(map(p, b), flags);
}
int posix_spawn(pid_t *pid, const char *p, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[]) {
    REAL(int, posix_spawn, pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
         char *const[], char *const[]);
    char b[PATH_MAX];
    return real_posix_spawn(pid, map(p, b), fa, at, argv, envp);
}

// ---- temporary files: the name is a pattern the function fills in -----------------------
// Chromium-based guests make their shared memory with mkstemp("/dev/shm/.XXX.XXXXXX").
// The caller gets the name it would have had without the mapping: it ends the same way.
static void name_back(char *pattern, const char *mapped) {
    size_t n = strlen(pattern), m = strlen(mapped);
    if (m >= n) memcpy(pattern, mapped + (m - n), n);
}
int mkstemp(char *t) {
    REAL(int, mkstemp, char *);
    char b[PATH_MAX];
    if (map(t, b) == t) return real_mkstemp(t);
    int r = real_mkstemp(b);
    name_back(t, b);
    return r;
}
int mkstemp64(char *t) {
    REAL(int, mkstemp64, char *);
    char b[PATH_MAX];
    if (map(t, b) == t) return real_mkstemp64(t);
    int r = real_mkstemp64(b);
    name_back(t, b);
    return r;
}
int mkostemp(char *t, int flags) {
    REAL(int, mkostemp, char *, int);
    char b[PATH_MAX];
    if (map(t, b) == t) return real_mkostemp(t, flags);
    int r = real_mkostemp(b, flags);
    name_back(t, b);
    return r;
}
int mkostemp64(char *t, int flags) {
    REAL(int, mkostemp64, char *, int);
    char b[PATH_MAX];
    if (map(t, b) == t) return real_mkostemp64(t, flags);
    int r = real_mkostemp64(b, flags);
    name_back(t, b);
    return r;
}
int mkstemps(char *t, int suffix) {
    REAL(int, mkstemps, char *, int);
    char b[PATH_MAX];
    if (map(t, b) == t) return real_mkstemps(t, suffix);
    int r = real_mkstemps(b, suffix);
    name_back(t, b);
    return r;
}
char *mkdtemp(char *t) {
    REAL(char *, mkdtemp, char *);
    char b[PATH_MAX];
    if (map(t, b) == t) return real_mkdtemp(t);
    char *r = real_mkdtemp(b);
    name_back(t, b);
    return r ? t : NULL;
}

// ---- *at variants -------------------------------------------------------------------
int faccessat(int d, const char *p, int m, int f) {
    REAL(int, faccessat, int, const char *, int, int);
    char b[PATH_MAX];
    return real_faccessat(d, map(p, b), m, f);
}
int fstatat(int d, const char *p, struct stat *s, int f) {
    REAL(int, fstatat, int, const char *, struct stat *, int);
    char b[PATH_MAX];
    return real_fstatat(d, map(p, b), s, f);
}
int fstatat64(int d, const char *p, struct stat64 *s, int f) {
    REAL(int, fstatat64, int, const char *, struct stat64 *, int);
    char b[PATH_MAX];
    return real_fstatat64(d, map(p, b), s, f);
}
int statx(int d, const char *p, int f, unsigned int m, struct statx *s) {
    REAL(int, statx, int, const char *, int, unsigned int, struct statx *);
    char b[PATH_MAX];
    return real_statx(d, map(p, b), f, m, s);
}
int mkdirat(int d, const char *p, mode_t m) {
    REAL(int, mkdirat, int, const char *, mode_t);
    char b[PATH_MAX];
    return real_mkdirat(d, map(p, b), m);
}
int unlinkat(int d, const char *p, int f) {
    REAL(int, unlinkat, int, const char *, int);
    char b[PATH_MAX];
    return real_unlinkat(d, map(p, b), f);
}
ssize_t readlinkat(int d, const char *p, char *buf, size_t n) {
    REAL(ssize_t, readlinkat, int, const char *, char *, size_t);
    char b[PATH_MAX];
    return real_readlinkat(d, map(p, b), buf, n);
}

// ---- two-path functions --------------------------------------------------------------
// Android's SELinux policy forbids hard links for apps (EACCES). Lock-file code
// (Xvfb: write .tX0-lock, link() it to .X0-lock) only needs "create the target
// atomically if absent, with the same content": emulate that with O_EXCL + copy.
static int link_fallback(int da, const char *a, int db, const char *b) {
    int in = openat(da, a, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = openat(db, b, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { n = -1; break; }
    int e = errno;
    close(in);
    close(out);
    if (n < 0) { unlinkat(db, b, 0); errno = e; return -1; }
    return 0;
}

int link(const char *a, const char *b2) {
    REAL(int, link, const char *, const char *);
    char x[PATH_MAX], y[PATH_MAX];
    const char *ma = map(a, x), *mb = map(b2, y);
    int r = real_link(ma, mb);
    if (r != 0 && (errno == EACCES || errno == EPERM)) r = link_fallback(AT_FDCWD, ma, AT_FDCWD, mb);
    return r;
}
int symlink(const char *target, const char *linkpath) {
    // The target is stored verbatim (resolved later through this same shim).
    REAL(int, symlink, const char *, const char *);
    char y[PATH_MAX];
    return real_symlink(target, map(linkpath, y));
}
int rename(const char *a, const char *b2) {
    REAL(int, rename, const char *, const char *);
    char x[PATH_MAX], y[PATH_MAX];
    return real_rename(map(a, x), map(b2, y));
}
int renameat(int da, const char *a, int db, const char *b2) {
    REAL(int, renameat, int, const char *, int, const char *);
    char x[PATH_MAX], y[PATH_MAX];
    return real_renameat(da, map(a, x), db, map(b2, y));
}
int linkat(int da, const char *a, int db, const char *b2, int f) {
    REAL(int, linkat, int, const char *, int, const char *, int);
    char x[PATH_MAX], y[PATH_MAX];
    const char *ma = map(a, x), *mb = map(b2, y);
    int r = real_linkat(da, ma, db, mb, f);
    if (r != 0 && (errno == EACCES || errno == EPERM) && !(f & AT_EMPTY_PATH)) r = link_fallback(da, ma, db, mb);
    return r;
}

// ---- exec: the program path (argv untouched) ------------------------------------
int execve(const char *p, char *const argv[], char *const envp[]) {
    REAL(int, execve, const char *, char *const[], char *const[]);
    char b[PATH_MAX];
    return real_execve(map(p, b), argv, envp);
}
int execv(const char *p, char *const argv[]) {
    extern char **environ;
    return execve(p, argv, environ);
}
int execl(const char *p, const char *arg, ...) {
    // Used by Xvfb's Popen("/bin/sh", "sh", "-c", cmd, NULL).
    char *argv[64];
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    argv[n++] = (char *)arg;
    while (n < 63 && (argv[n] = va_arg(ap, char *)) != NULL) n++;
    va_end(ap);
    argv[n] = NULL;
    return execv(p, argv);
}

// ---- TCP sockets: which process owns which, for lsof (tools/lsof/fxlsof.c) -----------
// A guest UI checks the peer of its websocket with lsof, and Android lets an app read
// neither /proc/net/tcp nor NETLINK_SOCK_DIAG. FEX writes a record per socket for x86 guests
// (patches/fex/src/AndroidTcpRegistry.h); this writes the same records for native programs:
// one file per socket inode, "<pid> <local addr> <port> <remote addr> <port>\n".
static void tcp_format(const struct sockaddr_storage *ss, char *out, size_t len) {
    char ip[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
        port = ntohs(a->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
        port = ntohs(a->sin6_port);
    } else {
        snprintf(out, len, "- 0");
        return;
    }
    snprintf(out, len, "%s %u", ip, port);
}

// `remote`: the connect() address, while a non-blocking connect is still in progress.
static void record_tcp(int fd, const struct sockaddr *remote, socklen_t rlen) {
    if (!g_root_len) return;
    int saved = errno;
    struct sockaddr_storage local = {0}, peer = {0};
    socklen_t llen = sizeof local, plen = sizeof peer;
    int type = 0;
    socklen_t tlen = sizeof type;
    struct stat st;
    if (getsockname(fd, (struct sockaddr *)&local, &llen) != 0 ||
        (local.ss_family != AF_INET && local.ss_family != AF_INET6) ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) != 0 || type != SOCK_STREAM || fstat(fd, &st) != 0) {
        errno = saved;
        return;
    }
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) {
        memset(&peer, 0, sizeof peer);
        if (remote && rlen <= sizeof peer) memcpy(&peer, remote, rlen);
    }
    char dir[PATH_MAX], path[PATH_MAX + 32], tmp[PATH_MAX + 64], l[INET6_ADDRSTRLEN + 8], r[INET6_ADDRSTRLEN + 8], line[128];
    snprintf(dir, sizeof dir, "%s/usr/share/fex-emu/fexdroid-tcp", g_root);
    mkdir(dir, 0700);
    tcp_format(&local, l, sizeof l);
    tcp_format(&peer, r, sizeof r);
    int n = snprintf(line, sizeof line, "%d %s %s\n", getpid(), l, r);
    snprintf(path, sizeof path, "%s/%lu", dir, (unsigned long)st.st_ino);
    snprintf(tmp, sizeof tmp, "%s.%d", path, gettid());
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out >= 0) {
        int ok = n > 0 && write(out, line, (size_t)n) == n;
        close(out);
        if (!ok || rename(tmp, path) != 0) unlink(tmp);
    }
    errno = saved;
}

static int is_inet(const struct sockaddr *sa) {
    return sa && (sa->sa_family == AF_INET || sa->sa_family == AF_INET6);
}

int accept(int fd, struct sockaddr *sa, socklen_t *len) {
    REAL(int, accept, int, struct sockaddr *, socklen_t *);
    int r = real_accept(fd, sa, len);
    if (r >= 0) record_tcp(r, NULL, 0);
    return r;
}
int accept4(int fd, struct sockaddr *sa, socklen_t *len, int flags) {
    REAL(int, accept4, int, struct sockaddr *, socklen_t *, int);
    int r = real_accept4(fd, sa, len, flags);
    if (r >= 0) record_tcp(r, NULL, 0);
    return r;
}

// ---- AF_UNIX filesystem sockets (abstract names start with '\0' and pass) ----------
static const struct sockaddr *map_sun(const struct sockaddr *sa, socklen_t *len, struct sockaddr_un *tmp) {
    if (!sa || sa->sa_family != AF_UNIX || *len <= offsetof(struct sockaddr_un, sun_path)) return sa;
    const struct sockaddr_un *un = (const struct sockaddr_un *)sa;
    if (un->sun_path[0] != '/') return sa;
    char b[PATH_MAX];
    const char *m = map(un->sun_path, b);
    if (m == un->sun_path || strlen(m) >= sizeof tmp->sun_path) return sa;
    memset(tmp, 0, sizeof *tmp);
    tmp->sun_family = AF_UNIX;
    strcpy(tmp->sun_path, m);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(m) + 1);
    return (const struct sockaddr *)tmp;
}
int bind(int fd, const struct sockaddr *sa, socklen_t len) {
    REAL(int, bind, int, const struct sockaddr *, socklen_t);
    struct sockaddr_un t;
    const struct sockaddr *m = map_sun(sa, &len, &t);
    int r = real_bind(fd, m, len);
    if (r == 0 && is_inet(sa)) record_tcp(fd, NULL, 0);
    return r;
}
int connect(int fd, const struct sockaddr *sa, socklen_t len) {
    REAL(int, connect, int, const struct sockaddr *, socklen_t);
    struct sockaddr_un t;
    const struct sockaddr *m = map_sun(sa, &len, &t);
    int r = real_connect(fd, m, len);
    if (is_inet(sa) && (r == 0 || errno == EINPROGRESS)) record_tcp(fd, sa, len);
    return r;
}
