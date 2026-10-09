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
"$P" "$T/make_new_game.py" "$A/mixed" 2>/dev/null
# The speech bubbles' accented capitals (É È Ê À Â Ç Ù Û Î Ï Ô), used by translations (texdump_french:
# a run of the French translation with all of them on screen): made from the pack's HD letters.
"$P" "$T/make_accents.py" "$A/originals" "$A/mixed"
# The save files' location pictures (64x22, the bar menu's GAME pictures): the 4K pack's versions
# land on the wrong pictures (other places, faded copies), so the game's own are kept.
"$P" - "$A/originals" "$A/mixed" <<'PY'
import os, sys
from PIL import Image
originals, mixed = sys.argv[1], sys.argv[2]
removed = 0
for name in os.listdir(mixed):
    original = os.path.join(originals, name)
    if name.endswith('.png') and os.path.exists(original) and Image.open(original).size == (64, 22):
        os.remove(os.path.join(mixed, name)); removed += 1
print(f'left out {removed} location pictures')
PY
# Pieces whose 4K version is from a differently coloured picture: the opening's "Starring" title,
# where the strips holding STARRING and the top of the CONKER letters came out gold and orange
# over the original's purple. The game's own are kept.
for h in da5087ca2823f435 c2e43ec56e5f408a a8cf89deb84cfd87 a47972f843956c5a 920363c9f0363a34 \
         81f13ed17c30793f 7c3b76c59317639e 6e03c1508b1bd712 160a109c5ee9da9b; do
  rm -f "$A/mixed/$h.png"
done
"$P" "$T/make_thumb.py" "$A" "$R/icons_work/hd_icons_thumb.png"
"$P" "$T/make_pack.py" "$A/mixed" "$R/icons_work/conker_hd_icons.rtz" --id conker_hd_icons --name "HD Icons" --version 1.3.3 \
  --description "Sharper HUD, menu and text pictures, with the original colours and shapes kept. Textures by GameBeast92 (Conker's Bad Fur Day 4K Ultimate Texture Pack, github.com/GameBeast92), modified, under CC BY 4.0. See LICENSE.txt in the pack." \
  --short "Sharper HUD and menus" --author dahmedvall95 --author "GameBeast92 (4K texture artwork)" \
  --thumb "$R/icons_work/hd_icons_thumb.png" --license "$T/HD_ICONS_LICENSE.txt"
CFG=$(ls -d /mnt/c/Users/*/AppData/Local/ConkerBFDReloaded | head -1)
cp "$R/icons_work/conker_hd_icons.rtz" "$CFG/mods/" && echo "installed in $CFG/mods"
# The French version (HD Icons (French)): the same pictures with the French translation's menus,
# titles and logos (Djipi's graphics from emulation64.fr's patch, tools/packs/conker_fr.htc; used
# with permission) in place of the English ones. Not installed: it's for players of the French ROM.
"$P" "$T/make_french_pack.py" $W/tools/packs/conker_fr.htc "$A/mixed" "$A/mixed_fr" $W/texdump* $W/scandump*
cat "$T/HD_ICONS_FR_LICENSE.txt" "$T/HD_ICONS_LICENSE.txt" > "$A/license_fr.txt"
"$P" "$T/make_pack.py" "$A/mixed_fr" "$R/icons_work/conker_hd_icons_fr.rtz" --id conker_hd_icons_fr --name "HD Icons (French)" --version 1.3.5 \
  --description "The HD Icons pack with the French translation's menus, titles, logos and many of the signs inside the levels (not all yet), for the French ROM (use it instead of HD Icons). French graphics by Djipi (emulation64.fr), used with permission. HD textures by GameBeast92, modified, under CC BY 4.0. See LICENSE.txt in the pack." \
  --short "HD Icons, French menus" --author dahmedvall95 --author "Djipi (French graphics)" --author "GameBeast92 (4K texture artwork)" \
  --thumb "$R/icons_work/hd_icons_thumb.png" --license "$A/license_fr.txt"
