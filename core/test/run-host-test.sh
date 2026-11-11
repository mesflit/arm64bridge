#!/usr/bin/env bash
# Builds the bridge for the build machine, compiles the 32-bit ARM demo library
# and runs it through the bridge under a desktop JVM.
# Needs: cmake, ninja, clang (with ARM target), lld, a JDK, zlib headers.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build/host-test}"
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
mkdir -p "$OUT/guest" "$OUT/shims" "$OUT/classes"

cmake -S "$ROOT/core" -B "$OUT/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ${A64B_CMAKE_ARGS:-}
cmake --build "$OUT/cmake"

clang --target=armv7a-linux-gnueabi -march=armv7-a -mfloat-abi=softfp -mfpu=neon -mthumb -O2 -fPIC \
      -ffreestanding -fno-builtin -fuse-init-array -nostdlib -shared -fuse-ld=lld -Wl,--hash-style=both \
      -isystem "$ROOT/core/guest/fakeinc" -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
      "$ROOT/core/guest/guestdemo.c" -o "$OUT/guest/libguestdemo.so"

# The shim takes the name of the library it stands in for.
cp "$OUT/cmake/liba64bshim.so" "$OUT/shims/libguestdemo.so"

javac -d "$OUT/classes" "$ROOT/app/src/main/java/com/mesflit/arm64bridge/GuestDemo.java" "$ROOT/core/test/HostTest.java"

A64B_GUEST_DIR="$OUT/guest" LD_LIBRARY_PATH="$OUT/cmake" \
    java -Xss4m -cp "$OUT/classes" HostTest "$OUT/shims/libguestdemo.so"
