# ARM64Bridge runtime: libarm64bridge.so (emulator + loader + thunks) and the shim.
cmake_minimum_required(VERSION 3.22)
project(arm64bridge C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Unicorn engine (ARM guest only), built from source ----
include(FetchContent)
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(unicorn
  GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
  GIT_TAG 2.1.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unicorn)

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
  src/posix_extra_thunks.cpp)
target_include_directories(arm64bridge PRIVATE src ${unicorn_SOURCE_DIR}/include)
set(A64B_VERSION "dev" CACHE STRING "Version string reported by the runtime")
target_compile_definitions(arm64bridge PRIVATE A64B_VERSION="${A64B_VERSION}")
target_compile_options(arm64bridge PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-security
  -Wno-format-nonliteral -U_FORTIFY_SOURCE -fvisibility=hidden -fno-strict-aliasing)
target_link_libraries(arm64bridge PRIVATE unicorn z)

add_library(a64bshim SHARED shim/shim.c)
# No DT_SONAME: every copy of the shim must be identified by its own file name.
set_target_properties(a64bshim PROPERTIES NO_SONAME ON)
target_link_libraries(a64bshim PRIVATE arm64bridge)

if(ANDROID)
  target_link_libraries(arm64bridge PRIVATE log android EGL GLESv2 GLESv1_CM)
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
