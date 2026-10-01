#!/usr/bin/env bash
# Build the HD Icons pack (icons_work/conker_hd_icons.rtz) from everything collected so far,
# and install it in the player's Mods folder.
#   tools/textures/build_icon_pack.sh
# Sources (C:\ConkerRecompWin): texdump* (RT64 dumps from test runs with FLAGS=texdump, and
# texdump_play from collect_textures.bat), scandump* (scan_rdram.py over memory snapshots taken
# with FLAGS=rdramdump, or over rom_assets.py files: scandump_rom), tools/packs/conker4k.htc (GameBeast92's 4K pack). Work: icons_work/all.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
W=/mnt/c/ConkerRecompWin
P="$R/.venv/bin/python"
T="$R/tools/textures"
A="$R/icons_work/all"
PACK=$W/tools/packs/conker4k.htc
HASHER='C:\ConkerRecompWin\host\build\rt64\src\tools\texture_hasher\texture_hasher.exe'
mkdir -p "$A/originals" "$A/pack4k" "$A/places"
: > "$A/places/rect_places.txt"
n=0
for d in $W/texdump* $W/scandump*; do
  [ -d "$d" ] || continue
  name=$(basename "$d")
  echo "== $name"
  (cd $W && cmd.exe /c "$HASHER $name --rice" >/dev/null 2>&1)
  # Each dump's pictures (2D only for real dumps; the scan has only 2D kinds).
  rm -rf "$A/tmp" && mkdir -p "$A/tmp"
  if [ "${name#scandump}" != "$name" ]; then "$P" "$T/decode_dump.py" "$d" "$A/tmp"; else "$P" "$T/decode_dump.py" "$d" "$A/tmp" --rect-only; fi
  ls "$A/tmp" | grep '\.png$' | sed 's/\.png$//' > "$A/tmp/hashes.txt"
  "$P" "$T/htc_extract.py" "$PACK" "$d/rt64.json" "$A/pack4k" --only-file "$A/tmp/hashes.txt"
  "$P" - "$A/tmp/index.json" "$A/originals/index.json" <<'PY'
import json, os, sys
new, path = json.load(open(sys.argv[1])), sys.argv[2]
old = json.load(open(path)) if os.path.exists(path) else {}
old.update(new); json.dump(old, open(path, 'w'), indent=1)
PY
  cp "$A"/tmp/*.png "$A/originals/" 2>/dev/null || true
  # Where pictures were drawn, each dump's frames kept apart.
  [ -f "$d/rect_places.txt" ] && sed "s/^/$n./" "$d/rect_places.txt" >> "$A/places/rect_places.txt"
  n=$((n + 1))
done
rm -rf "$A/tmp"
echo "== putting pictures together"
"$P" "$T/tile_layout.py" "$A/places" "$A/originals" "$A/layout.json" $(for d in $W/scandump*; do printf -- '--memory %s ' "$d"; done)
rm -rf "$A/mixed"
"$P" "$T/mix_pack.py" "$A/originals" "$A/pack4k" "$A/layout.json" "$A/places/rect_places.txt" "$A/mixed" \
  --restyle 99779e,e70120 --redraw 46c2ab,186ece 2>/dev/null | tail -1
"$P" "$T/make_thumb.py" "$A" "$R/icons_work/hd_icons_thumb.png"
"$P" "$T/make_pack.py" "$A/mixed" "$R/icons_work/conker_hd_icons.rtz" --id conker_hd_icons --name "HD Icons" --version 1.2.0 \
  --description "Sharper HUD, menu and text pictures, with the original colours and shapes kept. Textures by GameBeast92 (Conker's Bad Fur Day 4K Ultimate Texture Pack, github.com/GameBeast92), modified, under CC BY 4.0. See LICENSE.txt in the pack." \
  --short "Sharper HUD and menus" --author dahmedvall95 --author "GameBeast92 (4K texture artwork)" \
  --thumb "$R/icons_work/hd_icons_thumb.png" --license "$T/HD_ICONS_LICENSE.txt"
CFG=$(ls -d /mnt/c/Users/*/AppData/Local/ConkerBFDReloaded | head -1)
cp "$R/icons_work/conker_hd_icons.rtz" "$CFG/mods/" && echo "installed in $CFG/mods"
