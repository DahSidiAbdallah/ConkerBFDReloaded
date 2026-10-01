#!/usr/bin/env bash
# Linux: fetches clean copies of the runtime, RT64 and RecompFrontend at the versions
# CBFD-Recompiled (sciaschi, MIT) uses, then applies the patches in patches/ (the same as
# host/setup_deps.bat does on Windows). Safe to re-run.
#   tools/setup_deps.sh [DEPS_DIR]      (default: deps/ in this folder)
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
D=$(realpath -m "${1:-$R/deps}")
P="$R/patches"
mkdir -p "$D"

get() { # name url commit
  [ -d "$D/$1/.git" ] || git clone -q "$2" "$D/$1"
  git -C "$D/$1" fetch -q origin "$3" 2>/dev/null || true
  git -C "$D/$1" checkout -q "$3"
  git -C "$D/$1" submodule update --init --recursive -q
  echo "  got $1 at $3"
}
patch_in() { # dir patch
  if git -C "$1" apply --reverse --check --ignore-whitespace "$2" >/dev/null 2>&1; then
    echo "  already patched $1 ($(basename "$2"))"
  else
    git -C "$1" apply --ignore-whitespace "$2" && echo "  patched $1 ($(basename "$2"))"
  fi
}

get N64ModernRuntime https://github.com/N64Recomp/N64ModernRuntime.git cdf5abbd5026fef5c364c676e4667c45e42b6863
get rt64 https://github.com/rt64/rt64.git 43373749dac9bbc1b653e6a02aed40a9e1783bed
get RecompFrontend https://github.com/N64Recomp/RecompFrontend.git b1a1477c6556aeb7ed45defbfb5924f721efebc1
patch_in "$D/N64ModernRuntime" "$P/n64modernruntime.patch"
patch_in "$D/rt64" "$P/rt64.patch"
patch_in "$D/N64ModernRuntime" "$P/n64modernruntime_conker.patch"
patch_in "$D/rt64" "$P/rt64_conker.patch"
patch_in "$D/RecompFrontend/recompui/lib/RmlUi" "$P/rmlui.patch"
patch_in "$D/RecompFrontend" "$P/recompfrontend_conker.patch"
patch_in "$D/RecompFrontend" "$P/recompfrontend.patch"
# RecompFrontend includes a header from ../patches next to the deps folder.
[ -e "$D/../patches" ] || ln -s "$P" "$D/../patches"
chmod +x "$D/rt64/src/contrib/dxc/bin/x64/dxc-linux"
echo "DEPS OK ($D)"
