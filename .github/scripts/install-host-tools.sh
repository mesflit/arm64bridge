#!/usr/bin/env bash
# Installs what the bridge tests need on a GitHub runner. The Ubuntu package mirrors are sometimes
# unreachable from the runners (DNS failures); in that case use what the runner image already has
# and fetch the rest from PyPI / upstream, instead of failing the build.
set -uo pipefail

APT_OPTS=(-o Acquire::Retries=2 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20)
if timeout 300 sudo apt-get "${APT_OPTS[@]}" update &&
   timeout 300 sudo apt-get "${APT_OPTS[@]}" install -y ninja-build clang lld zlib1g-dev libboost-dev; then
  echo "Installed build tools from the Ubuntu archive."
  exit 0
fi

echo "::warning::Ubuntu package mirror unavailable; using the runner image's tools instead."
BIN="$HOME/.local/a64b-bin"
mkdir -p "$BIN"
echo "$BIN" >> "$GITHUB_PATH"
export PATH="$BIN:$PATH"

command -v ninja >/dev/null || python3 -m pip install --user --quiet ninja || pipx install ninja || true
[ -x "$HOME/.local/bin/ninja" ] && ln -sf "$HOME/.local/bin/ninja" "$BIN/ninja"

# Versioned LLVM tools shipped with the image (clang-18, ld.lld-18, ...), newest first.
link_newest() {  # link_newest <name> <versioned prefix>
  command -v "$1" >/dev/null && return 0
  for v in 20 19 18 17 16 15; do
    if command -v "$2-$v" >/dev/null; then ln -sf "$(command -v "$2-$v")" "$BIN/$1"; return 0; fi
    if [ -x "/usr/lib/llvm-$v/bin/$1" ]; then ln -sf "/usr/lib/llvm-$v/bin/$1" "$BIN/$1"; return 0; fi
  done
  echo "::error::$1 not found on this runner"; return 1
}
link_newest clang clang
link_newest clang++ clang++
link_newest ld.lld ld.lld
# Boost headers are fetched by CMake when they are not installed (see core/CMakeLists.txt).
ninja --version && clang --version | head -1 && ld.lld --version
