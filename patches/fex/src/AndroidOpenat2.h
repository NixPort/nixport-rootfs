// SPDX-License-Identifier: MIT
// fexdroid (local patch, not upstream): userspace openat2() for Android.
//
// Android's app seccomp filter traps openat2 with SIGSYS (our glibc's syscall()
// turns that into ENOSYS). FEX opens every guest path through the x86 RootFS with
// openat2(RESOLVE_IN_ROOT), so it is emulated here: the path is walked component
// by component below `dirfd`; ".." and absolute symlinks never leave it.
#pragma once

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

namespace FEXDroid {

class DirStack {
public:
  explicit DirStack(int Root) { Fds.push_back(Root); }
  ~DirStack() {
    for (int Fd : Fds) {
      ::close(Fd);
    }
  }
  int Top() const { return Fds.back(); }
  void Push(int Fd) { Fds.push_back(Fd); }
  void Pop() {
    if (Fds.size() > 1) {
      ::close(Fds.back());
      Fds.pop_back();
    }
  }
  void ToRoot() {
    while (Fds.size() > 1) {
      Pop();
    }
  }
private:
  fextl::vector<int> Fds;
};

// Resolves Path beneath RootFd with RESOLVE_IN_ROOT semantics, then opens it.
inline int OpenInRoot(int RootFd, const char* Path, int Flags, mode_t Mode) {
  int Root = RootFd == AT_FDCWD ? ::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC) : ::fcntl(RootFd, F_DUPFD_CLOEXEC, 0);
  if (Root < 0) {
    return -1;
  }
  DirStack Dirs(Root);
  fextl::string Rest = Path;
  int Links = 0;

  auto OpenHere = [&](const char* Name, int ExtraFlags) {
    int Fd = ::openat(Dirs.Top(), Name, Flags | ExtraFlags, Mode);
    return Fd;
  };

  for (;;) {
    size_t Start = Rest.find_first_not_of('/');
    if (Start == fextl::string::npos) {
      // Path exhausted (or it was "/"): open the current directory itself.
      return OpenHere(".", 0);
    }
    Rest.erase(0, Start);
    size_t Slash = Rest.find('/');
    fextl::string Comp = Rest.substr(0, Slash);
    Rest = Slash == fextl::string::npos ? fextl::string {} : Rest.substr(Slash);
    const bool Last = Rest.find_first_not_of('/') == fextl::string::npos;
    const bool TrailingSlash = Last && !Rest.empty();

    if (Comp == ".") {
      if (Last) {
        return OpenHere(".", 0);
      }
      continue;
    }
    if (Comp == "..") {
      Dirs.Pop(); // never above the root
      if (Last) {
        return OpenHere(".", 0);
      }
      continue;
    }

    struct stat St {};
    const int StatResult = ::fstatat(Dirs.Top(), Comp.c_str(), &St, AT_SYMLINK_NOFOLLOW);
    const bool FollowLink = !Last || TrailingSlash || !(Flags & O_NOFOLLOW);
    if (StatResult == 0 && S_ISLNK(St.st_mode) && FollowLink) {
      if (++Links > 40) {
        errno = ELOOP;
        return -1;
      }
      char Target[PATH_MAX];
      ssize_t Len = ::readlinkat(Dirs.Top(), Comp.c_str(), Target, sizeof(Target) - 1);
      if (Len <= 0) {
        if (Len == 0) {
          errno = ENOENT;
        }
        return -1;
      }
      Target[Len] = '\0';
      if (Target[0] == '/') {
        Dirs.ToRoot();
      }
      Rest = fextl::string(Target) + (Rest.empty() ? fextl::string {} : Rest);
      if (Rest.find_first_not_of('/') == fextl::string::npos && Target[0] == '/') {
        return OpenHere(".", 0);
      }
      // A relative target is resolved from the directory holding the link.
      if (Target[0] != '/') {
        Rest = "/" + Rest;
      }
      continue;
    }

    if (Last) {
      return OpenHere(Comp.c_str(), TrailingSlash ? O_DIRECTORY : 0);
    }
    if (StatResult != 0) {
      return -1;
    }
    if (!S_ISDIR(St.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
    int Fd = ::openat(Dirs.Top(), Comp.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (Fd < 0) {
      return -1;
    }
    Dirs.Push(Fd);
  }
}

// Drop-in for ::syscall(SYSCALL_DEF(openat2), ...): returns an fd or -1/errno.
// HowT is the kernel's struct open_how or FEX's own FEX::HLE::open_how (same layout).
template<typename HowT>
inline int OpenAt2(int DirFd, const char* Path, const HowT* How, size_t Size) {
  if (Size < 24) { // OPEN_HOW_SIZE_VER0
    errno = EINVAL;
    return -1;
  }
  if (How->resolve == 0) {
    return ::openat(DirFd, Path, static_cast<int>(How->flags), static_cast<mode_t>(How->mode));
  }
  if (How->resolve == RESOLVE_IN_ROOT) {
    return OpenInRoot(DirFd, Path, static_cast<int>(How->flags), static_cast<mode_t>(How->mode));
  }
  // Other RESOLVE_* flags are not emulated; callers treat ENOSYS as "no openat2".
  errno = ENOSYS;
  return -1;
}

} // namespace FEXDroid
