# ARM64Bridge runtime: libarm64bridge.so (recompiler + loader + thunks) and the shim.
cmake_minimum_required(VERSION 3.22)
project(arm64bridge C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- dynarmic (ARM -> host dynamic recompiler) ----
option(A64B_SYSTEM_DYNARMIC "Use an installed dynarmic instead of building it" OFF)
if(A64B_SYSTEM_DYNARMIC)
  find_library(DYNARMIC_LIBRARY dynarmic REQUIRED)
  add_library(dynarmic INTERFACE)
  target_link_libraries(dynarmic INTERFACE ${DYNARMIC_LIBRARY})
else()
  include(FetchContent)
  if(NOT ANDROID)
    find_package(Boost 1.57 QUIET)
  endif()
  if(ANDROID OR NOT Boost_FOUND)
    # dynarmic needs the Boost headers; the NDK has none (and a desktop may not either).
    FetchContent_Declare(boost_headers
      URL https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.bz2)
    FetchContent_Populate(boost_headers)
    set(Boost_INCLUDE_DIR "${boost_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(BOOST_ROOT "${boost_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
  endif()
  set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
  set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(dynarmic
    GIT_REPOSITORY https://github.com/yuzu-mirror/dynarmic.git
    GIT_TAG fa6cc2e4b2a2954f2298b6548174479c5b106c2a)  # dynarmic 6.7.0
  FetchContent_MakeAvailable(dynarmic)
endif()

add_library(arm64bridge SHARED
  src/log.cpp
  src/guest_mem.cpp
  src/cpu.cpp
  src/symbols.cpp
  src/elf_loader.cpp
  src/libc_thunks.cpp
  src/stdio_thunks.cpp
  src/math_thunks.cpp
  src/pthread_thunks.cpp
  src/misc_thunks.cpp
  src/android_thunks.cpp
  src/gles_thunks.cpp
  src/jni_bridge.cpp
  src/trampoline.S
  src/zip.cpp
  src/api.cpp
  src/crash.cpp
  src/zlib_thunks.cpp
  src/posix_extra_thunks.cpp
  src/opensl_thunks.cpp
  src/limiter_probe.cpp
  src/frame_cap.cpp
  src/dl_thunks.cpp
  src/old_bionic_thunks.cpp
  src/guest_signals.cpp)
target_include_directories(arm64bridge PRIVATE src)
set(A64B_VERSION "dev" CACHE STRING "Version string reported by the runtime")
target_compile_definitions(arm64bridge PRIVATE A64B_VERSION="${A64B_VERSION}")
target_compile_options(arm64bridge PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-security
  -Wno-format-nonliteral -U_FORTIFY_SOURCE -fvisibility=hidden -fno-strict-aliasing)
target_link_libraries(arm64bridge PRIVATE dynarmic z)

add_library(a64bshim SHARED shim/shim.c)
# No DT_SONAME: every copy of the shim must be identified by its own file name.
set_target_properties(a64bshim PROPERTIES NO_SONAME ON)
target_link_libraries(a64bshim PRIVATE arm64bridge)

if(ANDROID)
  target_link_libraries(arm64bridge PRIVATE log android EGL GLESv2 GLESv1_CM OpenSLES)
  target_link_options(arm64bridge PRIVATE -Wl,-z,max-page-size=16384)
  target_link_options(a64bshim PRIVATE -Wl,-z,max-page-size=16384)
else()
  # Desktop build, used by the automated tests (runs under a regular JVM).
  find_package(JNI REQUIRED)
  find_package(Threads REQUIRED)
  target_include_directories(arm64bridge PRIVATE ${JNI_INCLUDE_DIRS})
  target_include_directories(a64bshim PRIVATE ${JNI_INCLUDE_DIRS})
  target_link_libraries(arm64bridge PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
