#!/usr/bin/env bash
# Regenerate patches/rt64_conker.patch: our changes to RT64 (C:\ConkerRecompWin\deps\rt64) on top of
# RT64 at its pinned commit with CBFD-Recompiled's patches/rt64.patch. Works in its own clone,
# C:\ConkerRecompWin\rt64_patch_base (made if missing), and never runs git anywhere else.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
W=${CONKER_WIN_DIR:-/mnt/c/ConkerRecompWin}
DEPS=$W/deps/rt64
BASE=$W/rt64_patch_base
COMMIT=43373749dac9bbc1b653e6a02aed40a9e1783bed
if [ ! -d "$BASE/.git" ]; then
  git clone -q --no-checkout "$DEPS" "$BASE"
  git -C "$BASE" -c core.autocrlf=false checkout -q "$COMMIT"
  git -C "$BASE" apply --ignore-whitespace "$R/patches/rt64.patch"
  git -C "$BASE" add -A
  git -C "$BASE" -c user.name=patch -c user.email=patch@local commit -qm "RT64 + CBFD-Recompiled's rt64.patch"
fi
git -C "$BASE" reset -q --hard HEAD
git -C "$BASE" clean -qfd
# Every source file that differs from the base (line endings aside) or is new.
files=()
while IFS= read -r f; do
  rel=${f#"$DEPS/"}
  if [ -f "$BASE/$rel" ]; then
    tr -d '\r' < "$f" | cmp -s - "$BASE/$rel" || files+=("$rel")
  else
    files+=("$rel")
  fi
done < <(find "$DEPS/src/hle" "$DEPS/src/render" "$DEPS/src/shaders" "$DEPS/src/shared" "$DEPS/src/gbi" "$DEPS/src/common" "$DEPS/include" -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hlsl' -o -name '*.hlsli' \); echo "$DEPS/CMakeLists.txt")
for rel in "${files[@]}"; do
  mkdir -p "$(dirname "$BASE/$rel")"
  tr -d '\r' < "$DEPS/$rel" > "$BASE/$rel"
  git -C "$BASE" add -N "$rel"
done
git -C "$BASE" diff > "$W/rt64_conker.patch.new"
git -C "$BASE" reset -q --hard HEAD
git -C "$BASE" clean -qfd
git -C "$BASE" apply "$W/rt64_conker.patch.new"
git -C "$BASE" apply -R --check "$W/rt64_conker.patch.new"
git -C "$BASE" reset -q --hard HEAD
git -C "$BASE" clean -qfd
mv "$W/rt64_conker.patch.new" "$R/patches/rt64_conker.patch"
cp "$R/patches/rt64_conker.patch" "$W/patches/rt64_conker.patch"
echo "patches/rt64_conker.patch: ${#files[@]} files (applies forward and back)"
