# Native (build-machine) build of FEX's thunk generator.
#
# The main FEX build is a cross-compile for aarch64 (see scripts/build-fex.sh), but
# thunkgen has to *run* on the build machine and link the build machine's libclang.
# This wrapper builds only ThunkLibs/Generator with the host toolchain; build-fex.sh
# then hands the resulting binary to the cross build (THUNKGEN_EXE, see
# patches/fex/0001-thunkgen-prebuilt.patch).
cmake_minimum_required(VERSION 3.20)
project(fexdroid-thunkgen C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT FEX_SOURCE_DIR)
  message(FATAL_ERROR "FEX_SOURCE_DIR must point at the FEX source tree")
endif()
set(FEX_PROJECT_SOURCE_DIR "${FEX_SOURCE_DIR}")
# Same vendored fmt the FEX tree uses.
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${FEX_SOURCE_DIR}/External/fmt" fmt EXCLUDE_FROM_ALL)
add_subdirectory("${FEX_SOURCE_DIR}/ThunkLibs/Generator" thunkgen)
