// Loads every FEX host thunk library (arm64) with dlopen and looks up its export
// symbol (fexthunks_exports_lib<name>), the same way FEX's ThunkHandler_impl::LoadLib does. Run inside the staged
// rootfs under qemu (scripts/test-payload-qemu.sh) to catch missing NEEDED libs,
// wrong interpreters/rpaths and unresolved symbols before touching the phone.
// Usage: dlopen-thunks <HostThunks dir> [name...]   (default: vulkan drm asound wayland-client)
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <HostThunks dir> [name...]\n", argv[0]); return 2; }
  const char* defaults[] = {"vulkan", "drm", "asound", "wayland-client"};
  int n = argc > 2 ? argc - 2 : 4, fails = 0;
  for (int i = 0; i < n; i++) {
    const char* name = argc > 2 ? argv[i + 2] : defaults[i];
    char path[512], sym[128];
    snprintf(path, sizeof path, "%s/lib%s-host.so", argv[1], name);
    snprintf(sym, sizeof sym, "fexthunks_exports_lib%s", name);
    for (char* p = sym; *p; p++) if (*p == '-') *p = '_';
    void* h = dlopen(path, RTLD_LOCAL | RTLD_NOW);
    if (!h) { printf("FAIL dlopen %s: %s\n", path, dlerror()); fails++; continue; }
    void* s = dlsym(h, sym);
    if (!s) { printf("FAIL %s: %s\n", path, dlerror()); fails++; continue; }
    // Same call FEX makes after dlopen: runs fexldr_init_<lib>, which dlopens the real
    // host library by soname (libvulkan.so.1, ...) and dlsyms every thunked function.
    // NULL means the real library is missing or lacks a symbol.
    void* exports = ((void* (*)(void))s)();
    printf("%s %s: %s() -> %s\n", exports ? "ok  " : "FAIL", path, sym, exports ? "exports table" : "NULL (host lib init failed)");
    if (!exports) fails++;
  }
  return fails ? 1 : 0;
}
