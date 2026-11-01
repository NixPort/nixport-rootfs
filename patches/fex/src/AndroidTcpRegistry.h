// SPDX-License-Identifier: MIT
// fexdroid (local, never upstreamed): record which process owns each guest TCP socket.
//
// Android apps can read neither /proc/net/tcp nor NETLINK_SOCK_DIAG, so lsof/ss cannot
// map a local TCP connection to its process. Steam's client does exactly that (it runs
// `lsof -i TCP@127.0.0.1:<port>`) to check that the websocket peer of its UI is its own
// steamwebhelper. Every guest connect/bind/accept goes through FEX, so FEX writes one
// file per socket inode into <global data dir>/fexdroid-tcp/:
//   "<pid> <local addr> <local port> <remote addr> <remote port>\n"
// tools/lsof/fxlsof.c answers lsof queries from it and only reports sockets that
// /proc/<pid>/fd still shows as open (stale files are removed there).
#pragma once

#include <FEXCore/Config/Config.h>
#include <FEXCore/fextl/string.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FEXDroid {

// udev without uevents. An Android app may not open a NETLINK_KOBJECT_UEVENT socket (EACCES),
// so libudev cannot create its monitor. SDL3 then gives up on udev, unloads the library and
// tries again the next time it looks for controllers: Dota 2 did that 13 times a second on
// its main thread, 300 system calls each time (NOTES N-034). The guest gets a socket on
// which nothing ever arrives instead: hotplug events are missing, as they are anyway.
inline int64_t UeventStandIn(int domain, int type, int protocol, int64_t Result) {
  constexpr int NetlinkKobjectUevent = 15;
  if (Result == -1 && errno == EACCES && domain == AF_NETLINK && protocol == NetlinkKobjectUevent) {
    return ::socket(AF_UNIX, SOCK_DGRAM | (type & (SOCK_CLOEXEC | SOCK_NONBLOCK)), 0);
  }
  return Result;
}

// bind() of such a socket to a netlink address: nothing to do.
inline bool IsUeventStandInBind(int fd, const sockaddr* addr, socklen_t addrlen) {
  if (!addr || addrlen < sizeof(sa_family_t) || addr->sa_family != AF_NETLINK) {
    return false;
  }
  int Domain = 0;
  socklen_t Len = sizeof(Domain);
  return ::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &Domain, &Len) == 0 && Domain == AF_UNIX;
}

inline void FormatTcpAddr(const sockaddr_storage& ss, char* out, size_t len) {
  char ip[INET6_ADDRSTRLEN] = "?";
  unsigned port = 0;
  if (ss.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    port = ntohs(a->sin_port);
  } else if (ss.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
    inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
    port = ntohs(a->sin6_port);
  } else {
    snprintf(out, len, "- 0");
    return;
  }
  snprintf(out, len, "%s %u", ip, port);
}

// Records TCP socket `fd` of this process. `Remote` is the connect() address, used while
// a non-blocking connect is still in progress (getpeername fails until it completes).
inline void RecordTcpSocket(int fd, const sockaddr* Remote = nullptr, socklen_t RemoteLen = 0) {
  const int SavedErrno = errno;
  sockaddr_storage Local {}, Peer {};
  socklen_t LocalLen = sizeof(Local), PeerLen = sizeof(Peer);
  int Type = 0;
  socklen_t TypeLen = sizeof(Type);
  struct stat St {};
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&Local), &LocalLen) != 0 ||
      (Local.ss_family != AF_INET && Local.ss_family != AF_INET6) ||
      getsockopt(fd, SOL_SOCKET, SO_TYPE, &Type, &TypeLen) != 0 || Type != SOCK_STREAM || fstat(fd, &St) != 0) {
    errno = SavedErrno;
    return;
  }
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&Peer), &PeerLen) != 0) {
    Peer = {};
    if (Remote && RemoteLen <= sizeof(Peer)) {
      memcpy(&Peer, Remote, RemoteLen);
    }
  }
  static const fextl::string Dir = FEXCore::Config::GetDataDirectory(true) + "fexdroid-tcp/";
  mkdir(Dir.c_str(), 0700);
  char L[INET6_ADDRSTRLEN + 8], R[INET6_ADDRSTRLEN + 8], Line[128];
  FormatTcpAddr(Local, L, sizeof(L));
  FormatTcpAddr(Peer, R, sizeof(R));
  const int N = snprintf(Line, sizeof(Line), "%d %s %s\n", getpid(), L, R);
  char Path[512], Tmp[544];
  snprintf(Path, sizeof(Path), "%s%lu", Dir.c_str(), static_cast<unsigned long>(St.st_ino));
  snprintf(Tmp, sizeof(Tmp), "%s.%d", Path, gettid());
  const int Out = open(Tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (Out >= 0) {
    const bool Ok = N > 0 && write(Out, Line, N) == N;
    close(Out);
    if (!Ok || rename(Tmp, Path) != 0) {
      unlink(Tmp);
    }
  }
  errno = SavedErrno;
}

inline bool IsInetAddr(const sockaddr* Addr) {
  return Addr && (Addr->sa_family == AF_INET || Addr->sa_family == AF_INET6);
}

} // namespace FEXDroid
