#!/usr/bin/env python3
"""Regenerates patches/fex/0001-*.patch from exact-match edits on a pristine FEX tree.

Local build-system-only changes (never upstreamed):
  - THUNKGEN_EXE: use a natively built thunkgen in a cross-compiled FEX build
  - THUNKS_ENABLE_GL: skip the GL/EGL thunks (no libGL on the phone)
  - THUNKS_ENABLE_32BIT_GUEST: skip the i386 guest/host thunk variants (no i386 sysroot yet)

Usage: make-fex-patch.py <fex-src> <patch-out>   (the tree is edited in place; run
`git diff > patch && git checkout -- .` afterwards, see scripts/build-fex.sh header)
"""
import pathlib, subprocess, sys

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])

EDITS = {
"CMakeLists.txt": [
("""set(X86_DEV_ROOTFS "/" CACHE FILEPATH "Path to the sysroot used for cross-compiling for i686 and x86_64")
""",
"""set(X86_DEV_ROOTFS "/" CACHE FILEPATH "Path to the sysroot used for cross-compiling for i686 and x86_64")
# fexdroid: when the main build is itself a cross-compile, thunkgen cannot be built
# here (it must run on the build machine and link the build machine's libclang).
# Point THUNKGEN_EXE at a natively built thunkgen instead.
set(THUNKGEN_EXE "" CACHE FILEPATH "Prebuilt thunkgen executable to use instead of building ThunkLibs/Generator")
option(THUNKS_ENABLE_GL "Build the GL/EGL thunks (needs GL/EGL headers and libGL in both sysroots)" TRUE)
option(THUNKS_ENABLE_32BIT_GUEST "Build the 32-bit (i686) guest thunks and their host counterparts" TRUE)
"""),
("""  set(FEX_PROJECT_SOURCE_DIR ${PROJECT_SOURCE_DIR})
  add_subdirectory(ThunkLibs/Generator)
""",
"""  set(FEX_PROJECT_SOURCE_DIR ${PROJECT_SOURCE_DIR})
  if (THUNKGEN_EXE)
    add_executable(thunkgen IMPORTED GLOBAL)
    set_target_properties(thunkgen PROPERTIES IMPORTED_LOCATION "${THUNKGEN_EXE}")
    set(THUNKGEN_DEPENDS "")
  else()
    add_subdirectory(ThunkLibs/Generator)
    set(THUNKGEN_DEPENDS DEPENDS thunkgen)
  endif()
"""),
("""      "-DGENERATOR_EXE=$<TARGET_FILE:thunkgen>"
      "-DX86_DEV_ROOTFS=${X86_DEV_ROOTFS}"
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    DEPENDS thunkgen)

  ExternalProject_Add(guest-libs-32
""",
"""      "-DGENERATOR_EXE=$<TARGET_FILE:thunkgen>"
      "-DX86_DEV_ROOTFS=${X86_DEV_ROOTFS}"
      "-DTHUNKS_ENABLE_GL=${THUNKS_ENABLE_GL}"
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    ${THUNKGEN_DEPENDS})

  if (THUNKS_ENABLE_32BIT_GUEST)
  ExternalProject_Add(guest-libs-32
"""),
("""      "-DGENERATOR_EXE=$<TARGET_FILE:thunkgen>"
      "-DX86_DEV_ROOTFS=${X86_DEV_ROOTFS}"
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    DEPENDS thunkgen)

  install(
""",
"""      "-DGENERATOR_EXE=$<TARGET_FILE:thunkgen>"
      "-DX86_DEV_ROOTFS=${X86_DEV_ROOTFS}"
      "-DTHUNKS_ENABLE_GL=${THUNKS_ENABLE_GL}"
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    ${THUNKGEN_DEPENDS})
  endif()

  install(
"""),
("""  install(
    CODE "message(\\"-- Installing: guest-libs-32\\")"
    CODE "
      execute_process(COMMAND ${CMAKE_COMMAND} --build . --target install
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Guest_32)"
    DEPENDS guest-libs-32
    COMPONENT Runtime)
""",
"""  if (THUNKS_ENABLE_32BIT_GUEST)
  install(
    CODE "message(\\"-- Installing: guest-libs-32\\")"
    CODE "
      execute_process(COMMAND ${CMAKE_COMMAND} --build . --target install
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Guest_32)"
    DEPENDS guest-libs-32
    COMPONENT Runtime)
  endif()
"""),
("""  add_custom_target(uninstall_guest-libs-32
    COMMAND ${CMAKE_COMMAND} "--build" "." "--target" "uninstall"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Guest_32)

  add_dependencies(uninstall uninstall_guest-libs)
  add_dependencies(uninstall uninstall_guest-libs-32)
""",
"""  add_dependencies(uninstall uninstall_guest-libs)
  if (THUNKS_ENABLE_32BIT_GUEST)
  add_custom_target(uninstall_guest-libs-32
    COMMAND ${CMAKE_COMMAND} "--build" "." "--target" "uninstall"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Guest_32)
  add_dependencies(uninstall uninstall_guest-libs-32)
  endif()
"""),
],
"ThunkLibs/HostLibs/CMakeLists.txt": [
("""    DEPENDS "${SOURCE_FILE}"
    DEPENDS thunkgen
    COMMAND thunkgen "${SOURCE_FILE}" "${NAME}" "-host" "${OUTFILE}" "${X86_DEV_ROOTFS}" ${BITNESS_FLAGS} -- -std=c++20
""",
"""    DEPENDS "${SOURCE_FILE}"
    DEPENDS "$<TARGET_FILE:thunkgen>"
    COMMAND "$<TARGET_FILE:thunkgen>" "${SOURCE_FILE}" "${NAME}" "-host" "${OUTFILE}" "${X86_DEV_ROOTFS}" ${BITNESS_FLAGS} -- -std=c++20
"""),
("""set(BITNESS_LIST "32;64")
foreach(GUEST_BITNESS IN LISTS BITNESS_LIST)
  if (BUILD_FEX_LINUX_TESTS)
""",
"""set(BITNESS_LIST "64")
if (THUNKS_ENABLE_32BIT_GUEST)
  set(BITNESS_LIST "32;64")
endif()
foreach(GUEST_BITNESS IN LISTS BITNESS_LIST)
  if (BUILD_FEX_LINUX_TESTS)
"""),
("""  generate(libEGL ${CMAKE_CURRENT_SOURCE_DIR}/../libEGL/libEGL_interface.cpp ${GUEST_BITNESS})
""",
"""  if (THUNKS_ENABLE_GL)
  generate(libEGL ${CMAKE_CURRENT_SOURCE_DIR}/../libEGL/libEGL_interface.cpp ${GUEST_BITNESS})
"""),
("""  target_link_libraries(GL-host-${GUEST_BITNESS} PRIVATE OpenGL::GL)
""",
"""  target_link_libraries(GL-host-${GUEST_BITNESS} PRIVATE OpenGL::GL)
  endif()
"""),
],
"ThunkLibs/GuestLibs/CMakeLists.txt": [
("""generate(libGL ${CMAKE_CURRENT_SOURCE_DIR}/../libGL/libGL_interface.cpp)
""",
"""if (NOT DEFINED THUNKS_ENABLE_GL)
  set(THUNKS_ENABLE_GL TRUE)
endif()
if (THUNKS_ENABLE_GL)
generate(libGL ${CMAKE_CURRENT_SOURCE_DIR}/../libGL/libGL_interface.cpp)
"""),
("""target_link_libraries(GL-guest PRIVATE PlaceholderX11)
""",
"""target_link_libraries(GL-guest PRIVATE PlaceholderX11)
endif()
"""),
],
}

for rel, edits in EDITS.items():
    p = src / rel
    s = p.read_text()
    for before, after in edits:
        if s.count(before) != 1:
            sys.exit(f"{rel}: expected exactly one match for:\n{before}")
        s = s.replace(before, after)
    p.write_text(s)
diff = subprocess.run(["git", "-C", str(src), "diff"], capture_output=True, text=True, check=True).stdout
subprocess.run(["git", "-C", str(src), "checkout", "--", "."], check=True)
out.write_text(diff)
print(f"wrote {out} ({diff.count(chr(10))} lines)")
