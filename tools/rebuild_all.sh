#!/usr/bin/env bash
# Build the game: re-translate it (when recomp/conker.toml or the prepared ELF changed, or
# with --force), copy the code, host/, patches/ and the Windows scripts to the Windows build
# folder (C:\ConkerRecompWin), and build ConkerBFDReloaded.exe there.
#   tools/rebuild_all.sh [--force]
# The prepared ELF comes from tools/prepare_game.sh.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
W=${CONKER_WIN_DIR:-/mnt/c/ConkerRecompWin}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$R/recomp"
if [ "${1:-}" = "--force" ] || [ conker.toml -nt out/recomp_overlays.inl ] || [ conker.us.recomp.elf -nt out/recomp_overlays.inl ]; then
  echo "== translating"
  rm -rf out_new && mkdir out_new
  sed 's#^output_func_path = "out"#output_func_path = "out_new"#' conker.toml > .translate.toml
  N64RECOMP_KEEP_GOING=1 "$R/N64Recomp/build/N64Recomp" .translate.toml > translate.log 2>&1 || { tail translate.log; exit 1; }
  rm -f .translate.toml
  cp out/segments.c out_new/ && cp -r out/rsp out_new/
  rm -rf out && mv out_new out
  mkdir -p "$W/recomp" && rm -rf "$W/recomp/out"
  tar -cf "$T/out.tar" out && cp "$T/out.tar" "$W/recomp/" && (cd "$W/recomp" && cmd.exe /c "tar -xf out.tar" 2>/dev/null && rm -f out.tar)
fi
echo "== copying host"
cd "$R" && tar -cf "$T/host.tar" host patches && cp "$T/host.tar" "$W/" && (cd "$W" && cmd.exe /c "tar -xf host.tar" 2>/dev/null && rm -f host.tar)
for f in setup_deps build_host run_host collect_textures; do sed 's/$/\r/' "$R/host/$f.bat" > "$W/$f.bat"; done
for f in "$R"/tools/windows/*.ps1; do sed 's/$/\r/' "$f" > "$W/$(basename "$f")"; done
echo "== building"
cd "$W" && cmd.exe /c C:/ConkerRecompWin/build_host.bat > build_host.log 2>&1 || true
tr -d '\0' < build_host.log | grep -a -iE " error |FAILED:|BUILD OK" | head -10
