#!/usr/bin/env python3
"""Regenerates patches/fex/0002-android-seccomp.patch (local, never upstreamed).

Android's app seccomp filter kills a process with SIGSYS on syscalls outside its
allowlist. FEX forwards many guest syscalls to the host verbatim; this patch:
  * adds a constexpr table of trapped aarch64 syscalls (from AOSP policy files,
    scripts/lib/gen-android-seccomp.py) and makes every inline passthrough return
    -ENOSYS for them instead of executing `svc`;
  * routes guest shmget/shmctl through glibc (userspace SysV shm, fxshmd);
  * routes guest semget/semop/semtimedop(_time64) through glibc (userspace SysV
    semaphores, fxshmd; semctl and the i386 ipc() multiplexer already use
    ::syscall(), which our glibc routes) and fixes the i386 direct semctl
    syscall (union semun is passed by value; IPC_64 may be set in cmd);
  * x86-64 accept -> accept4(..., 0);
  * x86-64 set/get_robust_list kept in FEX (like FEX already does for 32-bit
    guests) instead of registering with the kernel;
  * replaces raw openat2 calls in FileManagement with a userspace RESOLVE_IN_ROOT
    implementation (patches/fex/src/AndroidOpenat2.h);
  * bind/connect: AF_UNIX paths under guest-only dirs (/tmp) map into the RootFS;
  * TCP connect/bind/accept are recorded for tools/lsof (patches/fex/src/AndroidTcpRegistry.h);
  * a process configured with a directory RootFS keeps it instead of the FEXServer's;
  * hides host paths apps cannot read (/proc/bus/pci) from guests: ENOENT, and shows
    /sys/bus/pci as an empty tree (libpci exit()s without a working access method).
Raw ::syscall() sites are covered by our glibc's syscall() wrapper (glibc 0003).

usage: make-fex-android-patch.py <fex src (pristine for these files)> <aosp-dir> <unistd.h> <patch out>
"""
import difflib, pathlib, subprocess, sys

src, aosp, unistd, out = (pathlib.Path(a) for a in sys.argv[1:5])
here = pathlib.Path(__file__).resolve().parent
repo = here.parent.parent
LS = "Source/Tools/LinuxEmulation/LinuxSyscalls/"

table = subprocess.run([sys.executable, str(here / "gen-android-seccomp.py"), str(aosp), str(unistd), "cpp"],
                       check=True, capture_output=True, text=True).stdout

GUARD = """  if constexpr (FEXDroid::IsFakeSuccessOnAndroid(syscall_num)) {
    return 0; // fexdroid: setuid family, a no-op for a non-root app; the syscall is trapped.
  } else if constexpr (FEXDroid::IsTrappedOnAndroid(syscall_num)) {
    return -ENOSYS; // fexdroid: Android's seccomp filter would SIGSYS here.
  }
"""

SOCKADDR_IMPL = """  // fexdroid: AF_UNIX paths under guest-only directories map into the RootFS (FileManager::RootFSSocketAddr).
  REGISTER_SYSCALL_IMPL({name}, [](FEXCore::Core::CpuStateFrame* Frame, int sockfd, const struct sockaddr* addr, socklen_t addrlen) -> uint64_t {{
    struct sockaddr_un Un;
    uint32_t UnLen;
    if (FEX::HLE::_SyscallHandler->FM.RootFSSocketAddr(addr, addrlen, Un, UnLen)) {{
      addr = reinterpret_cast<const struct sockaddr*>(&Un);
      addrlen = UnLen;
    }}
    if (FEXDroid::IsUeventStandInBind(sockfd, addr, addrlen)) {{
      return 0; // fexdroid: udev's monitor socket (AndroidTcpRegistry.h)
    }}
    uint64_t Result = ::{name}(sockfd, addr, addrlen);
    if (FEXDroid::IsInetAddr(addr) && (Result == 0 || errno == EINPROGRESS)) {{
      FEXDroid::RecordTcpSocket(sockfd, addr, addrlen); // fexdroid: lsof for Steam (AndroidTcpRegistry.h)
    }}
    SYSCALL_ERRNO();
  }});
"""

ACCEPT4_IMPL = """  // fexdroid: record accepted TCP sockets for lsof (AndroidTcpRegistry.h).
  REGISTER_SYSCALL_IMPL(accept4, [](FEXCore::Core::CpuStateFrame* Frame, int fd, struct sockaddr* addr, socklen_t* addrlen, int flags) -> uint64_t {
    uint64_t Result = ::accept4(fd, addr, addrlen, flags);
    if (static_cast<int64_t>(Result) >= 0) {
      FEXDroid::RecordTcpSocket(static_cast<int>(Result));
    }
    SYSCALL_ERRNO();
  });
"""

def x32socket(text):
    for name, op in (("bind", "OP_BIND"), ("connect", "OP_CONNECT")):
        a = f"""    case {op}: {{
      Result = ::{name}(Arguments[0], reinterpret_cast<const struct sockaddr*>(Arguments[1]), Arguments[2]);
      break;
    }}"""
        b = f"""    case {op}: {{
      // fexdroid: AF_UNIX paths under guest-only directories (FileManager::RootFSSocketAddr).
      auto Addr = reinterpret_cast<const struct sockaddr*>(Arguments[1]);
      socklen_t AddrLen = Arguments[2];
      struct sockaddr_un Un;
      uint32_t UnLen;
      if (FEX::HLE::_SyscallHandler->FM.RootFSSocketAddr(Addr, AddrLen, Un, UnLen)) {{
        Addr = reinterpret_cast<const struct sockaddr*>(&Un);
        AddrLen = UnLen;
      }}
      if (FEXDroid::IsUeventStandInBind(Arguments[0], Addr, AddrLen)) {{
        return 0; // fexdroid: udev's monitor socket (AndroidTcpRegistry.h)
      }}
      Result = ::{name}(Arguments[0], Addr, AddrLen);
      if (FEXDroid::IsInetAddr(Addr) && (Result == 0 || errno == EINPROGRESS)) {{
        FEXDroid::RecordTcpSocket(Arguments[0], Addr, AddrLen);
      }}
      break;
    }}"""
        if text.count(a) != 1:
            sys.exit(f"x32/Socket.cpp: {op} anchor not found")
        text = text.replace(a, b)
    reps = [
        ("""      Result = ::socket(Arguments[0], Arguments[1], Arguments[2]);
      break;""",
         """      Result = ::socket(Arguments[0], Arguments[1], Arguments[2]);
      Result = FEXDroid::UeventStandIn(Arguments[0], Arguments[1], Arguments[2], Result); // fexdroid
      break;"""),
        ("""      Result = ::accept(Arguments[0], reinterpret_cast<struct sockaddr*>(Arguments[1]), reinterpret_cast<socklen_t*>(Arguments[2]));
      break;""",
         """      Result = ::accept(Arguments[0], reinterpret_cast<struct sockaddr*>(Arguments[1]), reinterpret_cast<socklen_t*>(Arguments[2]));
      if (static_cast<int64_t>(Result) >= 0) {
        FEXDroid::RecordTcpSocket(static_cast<int>(Result)); // fexdroid
      }
      break;"""),
        ("""      return ::accept4(Arguments[0], reinterpret_cast<struct sockaddr*>(Arguments[1]), reinterpret_cast<socklen_t*>(Arguments[2]), Arguments[3]);""",
         """      Result = ::accept4(Arguments[0], reinterpret_cast<struct sockaddr*>(Arguments[1]), reinterpret_cast<socklen_t*>(Arguments[2]), Arguments[3]);
      if (static_cast<int64_t>(Result) >= 0) {
        FEXDroid::RecordTcpSocket(static_cast<int>(Result)); // fexdroid
      }
      break;"""),
        ('#include "LinuxSyscalls/x64/Syscalls.h"\n', '#include "LinuxSyscalls/x64/Syscalls.h"\n#include "LinuxSyscalls/AndroidTcpRegistry.h" // fexdroid\n'),
    ]
    for a, b in reps:
        if text.count(a) != 1:
            sys.exit(f"x32/Socket.cpp: anchor not found: {a[:60]!r}")
        text = text.replace(a, b)
    return text

def passthrough(text):
    # Only the ARCHITECTURE_arm64 section (before the first #else) uses inline svc.
    arm64, sep, rest = text.partition("\n#else\n")
    for n in range(7):
        sig_start = f"uint64_t SyscallPassthrough{n}(FEXCore::Core::CpuStateFrame* Frame"
        i = arm64.index(sig_start)
        j = arm64.index("{\n", i) + 2
        arm64 = arm64[:j] + GUARD + arm64[j:]
    text = arm64 + sep + rest
    reps = [
        ("  REGISTER_SYSCALL_IMPL(connect, SyscallPassthrough3<SYSCALL_DEF(connect)>);\n", SOCKADDR_IMPL.format(name="connect")),
        ("  REGISTER_SYSCALL_IMPL(socket, SyscallPassthrough3<SYSCALL_DEF(socket)>);\n",
         "  // fexdroid: a stand-in for udev's uevent socket, which Android denies (AndroidTcpRegistry.h).\n"
         "  REGISTER_SYSCALL_IMPL(socket, [](FEXCore::Core::CpuStateFrame* Frame, int domain, int type, int protocol) -> uint64_t {\n"
         "    uint64_t Result = FEXDroid::UeventStandIn(domain, type, protocol, ::socket(domain, type, protocol));\n"
         "    SYSCALL_ERRNO();\n"
         "  });\n"),
        ("  REGISTER_SYSCALL_IMPL(bind, SyscallPassthrough3<SYSCALL_DEF(bind)>);\n", SOCKADDR_IMPL.format(name="bind")),
        ("  REGISTER_SYSCALL_IMPL(accept4, SyscallPassthrough4<SYSCALL_DEF(accept4)>);\n", ACCEPT4_IMPL),
        ('#include "LinuxSyscalls/x32/Syscalls.h"\n',
         '#include "LinuxSyscalls/x32/Syscalls.h"\n#include "LinuxSyscalls/AndroidSeccomp.h"\n#include "LinuxSyscalls/AndroidTcpRegistry.h"\n'),
        ('#include <sys/epoll.h>\n', '#include <sys/epoll.h>\n#include <sys/shm.h>\n#include <sys/sem.h>\n#include <sys/socket.h>\n#include <errno.h>\n'),
        ("  REGISTER_SYSCALL_IMPL(semget, SyscallPassthrough3<SYSCALL_DEF(semget)>);\n",
         "  // fexdroid: SysV semaphores via glibc (memfd sets from fxshmd + futexes); the syscalls are trapped.\n"
         "  REGISTER_SYSCALL_IMPL(semget, [](FEXCore::Core::CpuStateFrame* Frame, key_t key, int nsems, int semflg) -> uint64_t {\n"
         "    uint64_t Result = ::semget(key, nsems, semflg);\n"
         "    SYSCALL_ERRNO();\n"
         "  });\n"),
        ("    REGISTER_SYSCALL_IMPL_X64(semop, SyscallPassthrough3<SYSCALL_DEF(semop)>);\n",
         "    // fexdroid: via glibc (userspace SysV semaphores); the syscall is trapped.\n"
         "    REGISTER_SYSCALL_IMPL_X64(semop, [](FEXCore::Core::CpuStateFrame* Frame, int semid, struct sembuf* sops, size_t nsops) -> uint64_t {\n"
         "      uint64_t Result = ::semop(semid, sops, nsops);\n"
         "      SYSCALL_ERRNO();\n"
         "    });\n"),
        ("    REGISTER_SYSCALL_IMPL_X64(semtimedop, SyscallPassthrough4<SYSCALL_DEF(semtimedop)>);\n",
         "    // fexdroid: via glibc (userspace SysV semaphores); the syscall is trapped.\n"
         "    REGISTER_SYSCALL_IMPL_X64(\n"
         "      semtimedop, [](FEXCore::Core::CpuStateFrame* Frame, int semid, struct sembuf* sops, size_t nsops, const struct timespec* timeout) -> uint64_t {\n"
         "        uint64_t Result = ::semtimedop(semid, sops, nsops, timeout);\n"
         "        SYSCALL_ERRNO();\n"
         "      });\n"),
        ("    REGISTER_SYSCALL_IMPL_X32(semtimedop_time64, SyscallPassthrough4<SYSCALL_DEF(semtimedop)>);\n",
         "    // fexdroid: via glibc (userspace SysV semaphores); the syscall is trapped. The i386\n"
         "    // __kernel_timespec has the same layout as the host's struct timespec.\n"
         "    REGISTER_SYSCALL_IMPL_X32(\n"
         "      semtimedop_time64, [](FEXCore::Core::CpuStateFrame* Frame, int semid, struct sembuf* sops, size_t nsops, const struct timespec* timeout) -> uint64_t {\n"
         "        uint64_t Result = ::semtimedop(semid, sops, nsops, timeout);\n"
         "        SYSCALL_ERRNO();\n"
         "      });\n"),
        ("  REGISTER_SYSCALL_IMPL(shmget, SyscallPassthrough3<SYSCALL_DEF(shmget)>);\n"
         "  REGISTER_SYSCALL_IMPL(shmctl, SyscallPassthrough3<SYSCALL_DEF(shmctl)>);\n",
         "  // fexdroid: via glibc (userspace SysV shm over memfd + fxshmd); the syscalls are trapped.\n"
         "  REGISTER_SYSCALL_IMPL(shmget, [](FEXCore::Core::CpuStateFrame* Frame, key_t key, size_t size, int shmflg) -> uint64_t {\n"
         "    uint64_t Result = ::shmget(key, size, shmflg);\n"
         "    SYSCALL_ERRNO();\n"
         "  });\n"
         "  REGISTER_SYSCALL_IMPL(shmctl, [](FEXCore::Core::CpuStateFrame* Frame, int shmid, int cmd, struct shmid_ds* buf) -> uint64_t {\n"
         "    uint64_t Result = ::shmctl(shmid, cmd, buf);\n"
         "    SYSCALL_ERRNO();\n"
         "  });\n"),
        ("  REGISTER_SYSCALL_IMPL(mkdirat, SyscallPassthrough3<SYSCALL_DEF(mkdirat)>);\n",
         "  // fexdroid: new directories under a RootFS-only parent (e.g. /tmp on Android) go into the RootFS.\n"
         "  REGISTER_SYSCALL_IMPL(mkdirat, [](FEXCore::Core::CpuStateFrame* Frame, int dirfd, const char* pathname, mode_t mode) -> uint64_t {\n"
         "    uint64_t Result = FEX::HLE::_SyscallHandler->FM.Mkdirat(dirfd, pathname, mode);\n"
         "    SYSCALL_ERRNO();\n"
         "  });\n"),
        ("  REGISTER_SYSCALL_IMPL(unlinkat, SyscallPassthrough3<SYSCALL_DEF(unlinkat)>);\n",
         "  // fexdroid: what Mkdirat/Open created inside the RootFS is removed there too (rm uses unlinkat).\n"
         "  REGISTER_SYSCALL_IMPL(unlinkat, [](FEXCore::Core::CpuStateFrame* Frame, int dirfd, const char* pathname, int flags) -> uint64_t {\n"
         "    uint64_t Result = FEX::HLE::_SyscallHandler->FM.Unlinkat(dirfd, pathname, flags);\n"
         "    SYSCALL_ERRNO();\n"
         "  });\n"),
        ("    REGISTER_SYSCALL_IMPL_X64(accept, SyscallPassthrough3<SYSCALL_DEF(accept)>);\n",
         "    // fexdroid: plain accept is trapped on Android (bionic only uses accept4).\n"
         "    REGISTER_SYSCALL_IMPL_X64(accept, [](FEXCore::Core::CpuStateFrame* Frame, int fd, struct sockaddr* addr, socklen_t* addrlen) -> uint64_t {\n"
         "      uint64_t Result = ::accept4(fd, addr, addrlen, 0);\n"
         "      if (static_cast<int64_t>(Result) >= 0) {\n"
         "        FEXDroid::RecordTcpSocket(static_cast<int>(Result));\n"
         "      }\n"
         "      SYSCALL_ERRNO();\n"
         "    });\n"),
        ("    REGISTER_SYSCALL_IMPL_X64(set_robust_list, SyscallPassthrough2<SYSCALL_DEF(set_robust_list)>);\n"
         "    REGISTER_SYSCALL_IMPL_X64(get_robust_list, SyscallPassthrough3<SYSCALL_DEF(get_robust_list)>);\n",
         "    // fexdroid: set_robust_list is trapped on Android. Keep the head in FEX, as FEX\n"
         "    // already does for 32-bit guests; the kernel's cleanup on thread death is lost.\n"
         "    REGISTER_SYSCALL_IMPL_X64(set_robust_list, [](FEXCore::Core::CpuStateFrame* Frame, struct robust_list_head* head, size_t len) -> uint64_t {\n"
         "      if (len != 24) {\n"
         "        return -EINVAL;\n"
         "      }\n"
         "      auto ThreadObject = FEX::HLE::ThreadManager::GetStateObjectFromCPUState(Frame);\n"
         "      ThreadObject->ThreadInfo.robust_list_head = reinterpret_cast<uint64_t>(head);\n"
         "      return 0;\n"
         "    });\n"
         "    REGISTER_SYSCALL_IMPL_X64(\n"
         "      get_robust_list, [](FEXCore::Core::CpuStateFrame* Frame, int pid, struct robust_list_head** head, size_t* len_ptr) -> uint64_t {\n"
         "        if (pid != 0) {\n"
         "          return -EPERM;\n"
         "        }\n"
         "        FaultSafeUserMemAccess::VerifyIsWritable(head, sizeof(*head));\n"
         "        FaultSafeUserMemAccess::VerifyIsWritable(len_ptr, sizeof(*len_ptr));\n"
         "        auto ThreadObject = FEX::HLE::ThreadManager::GetStateObjectFromCPUState(Frame);\n"
         "        *head = reinterpret_cast<struct robust_list_head*>(ThreadObject->ThreadInfo.robust_list_head);\n"
         "        *len_ptr = 24;\n"
         "        return 0;\n"
         "      });\n"),
    ]
    for a, b in reps:
        if text.count(a) != 1:
            sys.exit(f"Passthrough.cpp: expected one match for {a[:60]!r}")
        text = text.replace(a, b)
    return text

FM_HELPERS = r"""
// fexdroid: host paths that exist on Android but that no app can read. Guests see
// them as missing, like on a machine without them. /proc/bus/pci: Android lists it but
// SELinux denies reading it; libpci's default error handler then calls exit(1), which
// kills Chromium's GPU process (steamwebhelper) at every start.
static bool FEXDroidIsHiddenHostPath(const char* pathname) {
  if (!pathname) {
    return false;
  }
  constexpr char Pci[] = "/proc/bus/pci";
  return strncmp(pathname, Pci, sizeof(Pci) - 1) == 0 && (pathname[sizeof(Pci) - 1] == '\0' || pathname[sizeof(Pci) - 1] == '/');
}

// fexdroid: most Android phones have no /tmp, /var/tmp, ... on the host, and where the
// host has such a directory (/etc everywhere, /tmp on some Android 16 phones) an app
// cannot write to it. Files and directories that guests create there would fail with
// ENOENT or EACCES although the RootFS has the directory. For an absolute path whose
// parent is missing on the host, or is there but not writable, and is present in the
// RootFS, returns the RootFS fd and the relative path to create there.
FileManager::EmulatedFDPathResult FileManager::GetRootFSCreatePath(const char* pathname, FDPathTmpData& Tmp, fextl::string& Rel) const {
  constexpr auto NoEntry = EmulatedFDPathResult {-1, nullptr};
  if (!pathname || pathname[0] != '/' || RootFSFD == AT_FDCWD) {
    return NoEntry;
  }
  fextl::string Full = pathname;
  while (Full.size() > 1 && Full.back() == '/') {
    Full.pop_back();
  }
  const auto Slash = Full.rfind('/');
  if (Slash == fextl::string::npos || Slash == 0 || Slash + 1 >= Full.size()) {
    return NoEntry;
  }
  const fextl::string Parent = Full.substr(0, Slash);
  if (::access(Parent.c_str(), W_OK | X_OK) == 0) {
    return NoEntry; // The host has the parent and lets us create things in it: normal behaviour.
  }
  auto P = GetEmulatedFDPath(AT_FDCWD, Parent.c_str(), true, Tmp);
  if (P.FD == -1 || P.FD == AT_FDCWD) {
    return NoEntry;
  }
  Rel = fextl::string(P.Path) + "/" + Full.substr(Slash + 1);
  return EmulatedFDPathResult {P.FD, Rel.c_str()};
}

// fexdroid: bind()/connect() pass AF_UNIX paths to the host verbatim, so a socket under
// a guest-only directory (/tmp on Android) fails with ENOENT. A path whose parent is missing
// on the host (or not writable there) but present in the RootFS is rewritten to that RootFS
// directory, the same rule Open/Mkdirat use for files. Both ends of a socket go through
// here, so they agree.
bool FileManager::RootFSSocketAddr(const void* Addr, uint32_t Len, struct sockaddr_un& Out, uint32_t& OutLen) const {
  constexpr auto PathOff = offsetof(struct sockaddr_un, sun_path);
  if (!Addr || Len <= PathOff || Len > sizeof(Out)) {
    return false;
  }
  const auto In = static_cast<const struct sockaddr_un*>(Addr);
  if (In->sun_family != AF_UNIX || In->sun_path[0] != '/') {
    return false; // Abstract or relative: nothing to map.
  }
  char Path[sizeof(Out.sun_path) + 1] {};
  memcpy(Path, In->sun_path, Len - PathOff);
  FDPathTmpData Tmp;
  fextl::string Rel;
  auto C = GetRootFSCreatePath(Path, Tmp, Rel);
  if (C.FD == -1) {
    return false;
  }
  char FDLink[64];
  snprintf(FDLink, sizeof(FDLink), "/proc/self/fd/%d", C.FD);
  char Root[PATH_MAX];
  const ssize_t N = ::readlink(FDLink, Root, sizeof(Root) - 1);
  if (N <= 0) {
    return false;
  }
  Root[N] = '\0';
  const fextl::string Full = fextl::string(Root) + "/" + C.Path;
  if (Full.size() >= sizeof(Out.sun_path)) {
    return false; // Would not fit: keep the ENOENT the guest gets anyway.
  }
  memset(&Out, 0, sizeof(Out));
  Out.sun_family = AF_UNIX;
  memcpy(Out.sun_path, Full.c_str(), Full.size());
  OutLen = PathOff + Full.size() + 1;
  return true;
}

// fexdroid: removing something that only exists inside the RootFS (created there by
// Mkdirat/Open above): resolve it to the RootFS fd + relative path.
uint64_t FileManager::Unlinkat(int dirfd, const char* pathname, int flags) {
  if (pathname && pathname[0] == '/') {
    struct stat st;
    if (::lstat(pathname, &st) != 0) {
      FDPathTmpData Tmp;
      auto P = GetEmulatedFDPath(AT_FDCWD, pathname, false, Tmp);
      if (P.FD != -1 && P.FD != AT_FDCWD) {
        return ::unlinkat(P.FD, P.Path, flags);
      }
    }
  }
  return ::unlinkat(dirfd, pathname, flags);
}

uint64_t FileManager::Mkdirat(int dirfd, const char* pathname, uint32_t mode) {
  if (pathname && pathname[0] == '/') {
    FDPathTmpData Tmp;
    fextl::string Rel;
    auto C = GetRootFSCreatePath(pathname, Tmp, Rel);
    if (C.FD != -1) {
      return ::mkdirat(C.FD, C.Path, mode);
    }
  }
  return ::mkdirat(dirfd, pathname, mode);
}

"""

HIDE_IN = ["Open(const char* pathname, int flags, uint32_t mode)", "Stat(const char* pathname, void* buf)",
           "Lstat(const char* pathname, void* buf)", "Access(const char* pathname, [[maybe_unused]] int mode)",
           "FAccessat(int dirfd, const char* pathname, int mode)",
           "FAccessat2(int dirfd, const char* pathname, int mode, int flags)",
           "Openat([[maybe_unused]] int dirfs, const char* pathname, int flags, uint32_t mode)",
           "Openat2(int dirfs, const char* pathname, FEX::HLE::open_how* how, size_t usize)",
           "Statx(int dirfd, const char* pathname, int flags, uint32_t mask, struct statx* statxbuf)",
           "NewFSStatAt(int dirfd, const char* pathname, struct stat* buf, int flag)",
           "NewFSStatAt64(int dirfd, const char* pathname, struct stat64* buf, int flag)"]

GETSELF_OLD = """  if (strcmp(Pathname, "/proc/self/exe") == 0 || strcmp(Pathname, "/proc/thread-self/exe") == 0 || strcmp(Pathname, PidSelfPath) == 0) {
    return Filename();
  }

  return Pathname;
}"""
GETSELF_NEW = """  if (strcmp(Pathname, "/proc/self/exe") == 0 || strcmp(Pathname, "/proc/thread-self/exe") == 0 || strcmp(Pathname, PidSelfPath) == 0) {
    return Filename();
  }

  // fexdroid: libpci exit()s when it finds no usable access method, and apps can read
  // neither /sys/bus/pci nor /proc/bus/pci (the latter is hidden above). Guests get an
  // empty sysfs PCI tree instead (<data dir>/fexdroid-empty-pci/devices/, from the payload):
  // a machine without PCI devices, which Chromium's GPU process accepts.
  constexpr char SysPci[] = "/sys/bus/pci";
  if (strncmp(Pathname, SysPci, sizeof(SysPci) - 1) == 0 && (Pathname[sizeof(SysPci) - 1] == '\\0' || Pathname[sizeof(SysPci) - 1] == '/')) {
    thread_local fextl::string Fake;
    Fake = FEXCore::Config::GetDataDirectory(true) + "fexdroid-empty-pci" + (Pathname + sizeof(SysPci) - 1);
    return Fake;
  }

  return Pathname;
}"""

def filemanagement(text):
    if text.count(GETSELF_OLD) != 1:
        sys.exit("FileManagement.cpp: GetSelf anchor not found")
    text = text.replace(GETSELF_OLD, GETSELF_NEW)
    for sig in HIDE_IN:
        a = f"uint64_t FileManager::{sig} {{\n"
        if text.count(a) != 1:
            sys.exit(f"FileManagement.cpp: anchor not found: {sig}")
        text = text.replace(a, a + "  if (FEXDroidIsHiddenHostPath(pathname)) { // fexdroid\n    errno = ENOENT;\n    return -1;\n  }\n")
    anchor = "uint64_t FileManager::Open(const char* pathname, int flags, uint32_t mode) {"
    if text.count(anchor) != 1:
        sys.exit("FileManagement.cpp: Open anchor not found")
    text = text.replace(anchor, FM_HELPERS + anchor)
    old_else = """    ReplaceEmuFd(fd, flags, mode);
  } else {
    fd = ::open(SelfPath, flags, mode);
  }

  return fd;
}"""
    new_else = """    ReplaceEmuFd(fd, flags, mode);
  } else {
    // fexdroid: creating/writing a file under a RootFS-only parent (see GetRootFSCreatePath).
    FDPathTmpData CreateTmp;
    fextl::string CreateRel;
    auto C = GetRootFSCreatePath(SelfPath, CreateTmp, CreateRel);
    if (C.FD != -1) {
      fd = ::openat(C.FD, C.Path, flags, mode);
    } else {
      fd = ::open(SelfPath, flags, mode);
    }
  }

  return fd;
}"""
    if text.count(old_else) != 1:
        sys.exit("FileManagement.cpp: Open else-branch anchor not found")
    text = text.replace(old_else, new_else)
    old_at = """    ReplaceEmuFd(fd, flags, mode);
  } else {
    fd = ::syscall(SYSCALL_DEF(openat), dirfs, SelfPath, flags, mode);
  }"""
    new_at = """    ReplaceEmuFd(fd, flags, mode);
  } else {
    // fexdroid: creating/writing a file under a RootFS-only parent (see GetRootFSCreatePath).
    FDPathTmpData CreateTmp;
    fextl::string CreateRel;
    auto C = GetRootFSCreatePath(SelfPath, CreateTmp, CreateRel);
    if (C.FD != -1) {
      fd = ::openat(C.FD, C.Path, flags, mode);
    } else {
      fd = ::syscall(SYSCALL_DEF(openat), dirfs, SelfPath, flags, mode);
    }
  }"""
    if text.count(old_at) != 1:
        sys.exit("FileManagement.cpp: Openat else-branch anchor not found")
    text = text.replace(old_at, new_at)
    n = text.count("::syscall(SYSCALL_DEF(openat2), ")
    if n != 6:
        sys.exit(f"FileManagement.cpp: expected 6 openat2 sites, found {n}")
    text = text.replace("::syscall(SYSCALL_DEF(openat2), ", "FEXDroid::OpenAt2(")
    a = '#include <linux/openat2.h>\n'
    if text.count(a) != 1:
        sys.exit("FileManagement.cpp: include anchor not found")
    return text.replace(a, a + '#include "LinuxSyscalls/AndroidOpenat2.h" // fexdroid\n')

def fs(text):
    reps = [
        ("    uint64_t Result = ::mkdir(pathname, mode);\n",
         "    uint64_t Result = FEX::HLE::_SyscallHandler->FM.Mkdirat(AT_FDCWD, pathname, mode); // fexdroid\n"),
        ("    uint64_t Result = ::rmdir(pathname);\n",
         "    uint64_t Result = FEX::HLE::_SyscallHandler->FM.Unlinkat(AT_FDCWD, pathname, AT_REMOVEDIR); // fexdroid\n"),
        ("    uint64_t Result = ::unlink(pathname);\n",
         "    uint64_t Result = FEX::HLE::_SyscallHandler->FM.Unlinkat(AT_FDCWD, pathname, 0); // fexdroid\n"),
        ("    uint64_t Result = ::creat(pathname, mode);\n",
         "    uint64_t Result = FEX::HLE::_SyscallHandler->FM.Open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode); // fexdroid\n"),
    ]
    for a, b in reps:
        if text.count(a) != 1:
            sys.exit(f"FS.cpp: anchor not found: {a.strip()}")
        text = text.replace(a, b)
    inc = "#include <fcntl.h>\n"
    return text if inc in text else text.replace("namespace FEX::HLE {", "#include <fcntl.h> // fexdroid\n\nnamespace FEX::HLE {", 1)

def x32semaphore(text):
    a = """  REGISTER_SYSCALL_IMPL_X32(semctl, [](FEXCore::Core::CpuStateFrame* Frame, int semid, int semnum, int cmd, semun_32* semun) -> uint64_t {
    uint64_t Result {};
    bool IPC64 = cmd & 0x100;
"""
    b = """  // fexdroid: the i386 semctl syscall passes union semun by value (only ipc(SEMCTL) passes a
  // pointer to it), and glibc built for kernels >= 5.1 sets IPC_64 in cmd.
  REGISTER_SYSCALL_IMPL_X32(semctl, [](FEXCore::Core::CpuStateFrame* Frame, int semid, int semnum, int cmd, uint32_t semun_raw) -> uint64_t {
    uint64_t Result {};
    bool IPC64 = cmd & 0x100;
    cmd &= 0xFF;
    semun_32 semun_val {};
    semun_val.val = semun_raw;
    semun_32* semun = &semun_val;
"""
    if text.count(a) != 1:
        sys.exit("x32/Semaphore.cpp: semctl anchor not found")
    return text.replace(a, b)

def serverclient(text):
    a = """  if (FEXCore::Config::FindContainer() != "pressure-vessel") {
    fextl::string RootFSPath = FEXServerClient::RequestRootFSPath(ServerFD);

    //// If everything has passed then we can now update the rootfs path
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ROOTFS, RootFSPath);
  }
"""
    b = """  // fexdroid: a RootFS that is a directory needs no mounting by the server, so keep the
  // one this process was configured with. Games started by Steam get FEX_ROOTFS pointed at
  // their runtime (tools/steam/_v2-entry-point) while the one FEXServer of the session
  // still serves Steam's own RootFS; its answer would undo that.
  FEX_CONFIG_OPT(ConfiguredRootFS, ROOTFS);
  struct stat RootFSStat {};
  const bool HasDirectoryRootFS =
    !ConfiguredRootFS().empty() && stat(ConfiguredRootFS().c_str(), &RootFSStat) == 0 && S_ISDIR(RootFSStat.st_mode);
  if (!HasDirectoryRootFS && FEXCore::Config::FindContainer() != "pressure-vessel") {
    fextl::string RootFSPath = FEXServerClient::RequestRootFSPath(ServerFD);

    //// If everything has passed then we can now update the rootfs path
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ROOTFS, RootFSPath);
  }
"""
    if text.count(a) != 1:
        sys.exit("FEXServerClient.cpp: SetupClient anchor not found")
    text = text.replace(a, b)
    inc = "#include <sys/socket.h>\n"
    if text.count(inc) != 1:
        sys.exit("FEXServerClient.cpp: include anchor not found")
    return text.replace(inc, inc + "#include <sys/stat.h> // fexdroid\n")

def fmheader(text):
    a = "  uint64_t FAccessat2(int dirfd, const char* pathname, int mode, int flags);\n"
    b = "  EmulatedFDPathResult GetEmulatedFDPath(int dirfd, const char* pathname, bool FollowSymlink, FDPathTmpData& TmpFilename) const;\n"
    if text.count(a) != 1 or text.count(b) != 1:
        sys.exit("FileManagement.h: anchors not found")
    text = text.replace(a, a + "  uint64_t Mkdirat(int dirfd, const char* pathname, uint32_t mode); // fexdroid\n"
                              "  uint64_t Unlinkat(int dirfd, const char* pathname, int flags); // fexdroid\n")
    text = text.replace(b, b + "  EmulatedFDPathResult GetRootFSCreatePath(const char* pathname, FDPathTmpData& Tmp, fextl::string& Rel) const; // fexdroid\n")
    text = text.replace(a, a + "  bool RootFSSocketAddr(const void* Addr, uint32_t Len, struct sockaddr_un& Out, uint32_t& OutLen) const; // fexdroid\n")
    inc = "#include <sys/stat.h>\n"
    if text.count(inc) != 1:
        sys.exit("FileManagement.h: include anchor not found")
    return text.replace(inc, inc + "#include <sys/socket.h> // fexdroid\n#include <sys/un.h> // fexdroid\n")

files = []
for rel, fn in ((LS + "Syscalls/Passthrough.cpp", passthrough), (LS + "FileManagement.cpp", filemanagement),
                (LS + "FileManagement.h", fmheader), (LS + "Syscalls/FS.cpp", fs),
                (LS + "x32/Semaphore.cpp", x32semaphore), (LS + "x32/Socket.cpp", x32socket),
                ("Source/Common/FEXServerClient.cpp", serverclient)):
    old = (src / rel).read_text()
    files.append((rel, old, fn(old)))
files.append((LS + "AndroidSeccomp.h", "", table))
files.append((LS + "AndroidOpenat2.h", "", (repo / "patches/fex/src/AndroidOpenat2.h").read_text()))
files.append((LS + "AndroidTcpRegistry.h", "", (repo / "patches/fex/src/AndroidTcpRegistry.h").read_text()))

chunks = []
for rel, old, new in files:
    chunks += difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                   f"a/{rel}" if old else "/dev/null", f"b/{rel}")
out.write_text("".join(chunks))
print(f"wrote {out}")
