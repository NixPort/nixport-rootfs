// The FEX side of the gamepad path (AndroidInputPad.h from FEX patch 0006), wrapped for
// tests/gamepad/hosttest.c. Built on the PC against the stubs in tests/gamepad/stubs.
#include "LinuxSyscalls/AndroidInputPad.h"

extern "C" {
int pc_map(const char* guest, char* out, size_t len) {
  fextl::string s;
  if (!FEXDroid::MapInputPath(guest, s)) {
    return 0;
  }
  snprintf(out, len, "%s", s.c_str());
  return 1;
}
int pc_number_of_mapped(const char* mapped) { return FEXDroid::PadNumberOfMappedPath(mapped); }
int pc_open(const char* mapped, int flags, int is64) { return FEXDroid::OpenPad(mapped, flags, is64 != 0); }
int pc_ioctl(int fd, unsigned cmd, void* arg, long* result) {
  uint64_t r = 0;
  if (!FEXDroid::PadIoctl(fd, cmd, reinterpret_cast<uint64_t>(arg), r)) {
    return 0;
  }
  *result = static_cast<long>(r);
  return 1;
}
void pc_fix_fd_stat(int fd, struct stat* st) { FEXDroid::PadFixFdStat(fd, st); }
}
