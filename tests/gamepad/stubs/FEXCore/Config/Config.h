// Stand-in for FEX's config API in tests/gamepad: the global data directory comes from
// $PADTEST_DATA_DIR (with a trailing slash, as FEX returns it).
#pragma once
#include <FEXCore/fextl/string.h>
#include <cstdlib>
namespace FEXCore::Config {
inline const fextl::string& GetDataDirectory(bool) {
  static const fextl::string Dir = std::getenv("PADTEST_DATA_DIR");
  return Dir;
}
} // namespace FEXCore::Config
