#!/usr/bin/env bash
# Builds the bridge (Rust) for the build machine, compiles the 32-bit ARM demo library
# and runs it through the bridge under a desktop JVM.
# Needs: cargo, clang (with ARM target), lld, a JDK, zlib.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build/host-test}"
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
mkdir -p "$OUT/guest" "$OUT/shims" "$OUT/classes"

cargo build --manifest-path "$ROOT/core/Cargo.toml" --release --workspace --target-dir "$OUT/cargo" ${A64B_CARGO_ARGS:-}
LIB="$OUT/cargo/release"

clang --target=armv7a-linux-gnueabi -march=armv7-a -mfloat-abi=softfp -mfpu=neon -mthumb -O2 -fPIC \
      -ffreestanding -fno-builtin -fuse-init-array -nostdlib -shared -fuse-ld=lld -Wl,--hash-style=both \
      -isystem "$ROOT/core/guest/fakeinc" -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
      "$ROOT/core/guest/guestdemo.c" -o "$OUT/guest/libguestdemo.so"

# The shim takes the name of the library it stands in for.
cp "$LIB/liba64bshim.so" "$OUT/shims/libguestdemo.so"

javac -d "$OUT/classes" "$ROOT/app/src/main/java/com/mesflit/arm64bridge/GuestDemo.java" "$ROOT/core/test/HostTest.java"

A64B_GUEST_DIR="$OUT/guest" LD_LIBRARY_PATH="$LIB" \
    java -Xss4m -cp "$OUT/classes" HostTest "$OUT/shims/libguestdemo.so"
