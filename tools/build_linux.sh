#!/usr/bin/env bash
# Linux: builds ConkerBFDReloaded (after tools/prepare_game.sh and a translation by
# tools/rebuild_all.sh, which make recomp/out, and tools/setup_deps.sh).
#   tools/build_linux.sh [DEPS_DIR] [BUILD_DIR]     (default: deps/ and build-linux/ here)
# Needs: CMake 3.20+, Ninja, GCC 13+ or Clang, and the development files of SDL2, GTK 3,
# FreeType, X11 and Vulkan (e.g. on Ubuntu: libsdl2-dev libgtk-3-dev libfreetype-dev
# libx11-dev libvulkan-dev).
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
D=$(realpath -m "${1:-$R/deps}")
B=$(realpath -m "${2:-$R/build-linux}")
[ -f "$R/recomp/out/funcs.h" ] || { echo "recomp/out is missing: run tools/prepare_game.sh and tools/rebuild_all.sh --force first"; exit 1; }
cmake -S "$R/host" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEPS_DIR="$D" -DRECOMP_OUT="$R/recomp/out"
# The translated game code is thousands of big files: one compile can take over 1 GB of memory,
# so run as many at once as the memory allows (about 1.5 GB each), not one per CPU core.
# Running out of memory makes WSL restart, losing the build. JOBS=N overrides.
mem_gb=$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo)
jobs=${JOBS:-$(( mem_gb * 2 / 3 ))}
[ "$jobs" -lt 1 ] && jobs=1
[ "$jobs" -gt "$(nproc)" ] && jobs=$(nproc)
echo "building with $jobs jobs at once (${mem_gb} GB free)"
ninja -C "$B" -j "$jobs"
echo "BUILD OK: $B/ConkerBFDReloaded"
