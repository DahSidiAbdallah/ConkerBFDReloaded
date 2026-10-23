#!/usr/bin/env bash
# One test run with some settings changed, screenshots kept under a name:
#   ./tools/testing/abrun.sh NAME "graphics:key=value sound:key=value ..." [secs ...]
# SNAP_ROOM=21 (hex): screenshot times are that room's timer (refreshes), to match the
# emulator (bizhawk/*.lua). FLAGS="cull43 ..." turns on C:\ConkerRecompWin\<name>.flag test switches for the run. NOSKIP=1 plays the intro instead. INPUT=... replaces the default presses. Uses Skip Intro and presses Start at 50 and 54 (save menu -> PLAY -> gameplay at ~58).
# Default screenshots: 47 (bar menu), 62 and 66 (gameplay). The settings files are backed
# up first and always put back (and the save file). Pictures: C:\ConkerRecompWin\snaps\ab\NAME\tN.png,
# log: ...\NAME\host_run.log, sound: ...\NAME\sound.wav. "seqps=8007B820,..." picks
# which sequence players count as music (experiments). MODS="name ..." or MODS=none picks the mods switched on. SPEED (default 4) fast-forwards after the skip.
set -u
name=$1; changes=$2; shift 2
secs="${*:-47 62 66}"
W=/mnt/c/ConkerRecompWin
CFG=$(ls -d /mnt/c/Users/*/AppData/Local/ConkerBFDReloaded | head -1)
out=$W/snaps/ab/$name; rm -rf $out; mkdir -p $out
touch $out/.start
# A run that was stopped before it could clean up left the player's own files as *.abtest:
# put them back before anything else, so they're never taken for test files.
if [ -f $CFG/graphics.json.abtest ]; then
  for f in graphics sound conker mods general accessibility; do [ -f $CFG/$f.json.abtest ] && mv -f $CFG/$f.json.abtest $CFG/$f.json; done
  [ -f $CFG/saves/save.abtest ] && mv -f $CFG/saves/save.abtest $CFG/saves/conker.n64.us.1.0.bin
  echo "(put back the settings and save a stopped run had left)"
fi
for f in graphics sound conker mods general accessibility; do cp $CFG/$f.json $CFG/$f.json.abtest; done
cp $CFG/saves/conker.n64.us.1.0.bin $CFG/saves/save.abtest # the player's save too
# Every run plays the same save: the player's 22 August emulator save (GAME1 continues
# outside the barn, room 0x0C). SAVE=name picks another from C:\ConkerRecompWin\emulator_saves
# (user_2026-09-30.eep: a copy of the player's own save, further on).
cp $W/emulator_saves/${SAVE:-pj64_2026-08-22.eep} $CFG/saves/conker.n64.us.1.0.bin
restore() { taskkill.exe /F /IM ConkerBFDReloaded.exe >/dev/null 2>&1; for f in graphics conker general accessibility; do cp -f $CFG/$f.json $out/settings_$f.json 2>/dev/null; done; touch $W/skip_intro.flag; mv -f $CFG/saves/save.abtest $CFG/saves/conker.n64.us.1.0.bin; for f in graphics sound conker mods general accessibility; do mv -f $CFG/$f.json.abtest $CFG/$f.json; done; rm -f $W/speed_until.txt $W/record.flag $W/music_seqps.txt $W/snap_room.txt $W/log_window.txt $W/test_warp.txt $W/test_place.txt $W/test_gun.txt $W/test_photo.txt $W/test_photo_shot.txt $W/test_photo_view.txt $W/test_state_save.txt $W/test_state_load.txt; for f in ${FLAGS:-}; do rm -f $W/$f.flag; done; }
trap restore EXIT
trap 'exit 130' INT TERM HUP
python3 - "$CFG" "$changes" <<'PY'
import json, sys
cfg, changes = sys.argv[1], sys.argv[2].split()
# Classic and Modern set their own settings at start-up, over the ones changed here: Custom keeps them.
if changes and not any(c.startswith('conker:experience=') for c in changes):
    changes = ['conker:experience=Custom'] + changes
for c in changes:
    if c.startswith('seqps='):
        open('/mnt/c/ConkerRecompWin/music_seqps.txt', 'w').write(c[6:]); continue
    file, kv = c.split(':', 1); key, value = kv.split('=', 1)
    p = f'{cfg}/{file}.json'; d = json.load(open(p))
    if value in ('true', 'false'): value = (value == 'true')
    else:
        try: value = float(value) if '.' in value else int(value)
        except ValueError: pass
    d[key] = value; json.dump(d, open(p, 'w'), indent=4)
PY
# MODS="a b" (or MODS=none): the mods switched on for the run, instead of the player's.
if [ -n "${MODS:-}" ]; then
  python3 - "$CFG/mods.json" "$MODS" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); d['enabled_mods'] = [] if sys.argv[2] == 'none' else sys.argv[2].split()
json.dump(d, open(sys.argv[1], 'w'), indent=4)
PY
fi
if [ -n "${NOSKIP:-}" ]; then rm -f $W/skip_intro.flag; else touch $W/skip_intro.flag; fi
for f in ${FLAGS:-}; do touch $W/$f.flag; done
rm -f $W/log_window.txt $W/snaps/*.txt $W/snaps/*.rdram; [ -n "${LOG_WINDOW:-}" ] && printf "$LOG_WINDOW" > $W/log_window.txt # room:from:to, for RT64's test logs
touch $W/record.flag; rm -f $W/snap_room.txt; [ -n "${SNAP_ROOM:-}" ] && printf "$SNAP_ROOM" > $W/snap_room.txt; rm -f $W/snaps/sound.wav
# WARP="seconds:room:entrance;..." (room in hex) sends Conker there, PLACE="seconds:x:y:z:facing;..." puts
# him at a spot (testing.cpp's CONKER_TEST_WARP / CONKER_TEST_PLACE): start tests anywhere.
# STATE_SAVE="seconds:name" saves a state at that game time to C:\ConkerRecompWin\states\name.state; STATE_LOAD=name
# loads one as soon as the game runs, so a test starts right there (host/src/save_states.cpp).
rm -f $W/test_state_save.txt $W/test_state_load.txt; mkdir -p $W/states
[ -n "${STATE_SAVE:-}" ] && printf '%s' "${STATE_SAVE%%:*}:C:/ConkerRecompWin/states/${STATE_SAVE#*:}.state" > $W/test_state_save.txt
[ -n "${STATE_LOAD:-}" ] && printf '%s' "C:/ConkerRecompWin/states/$STATE_LOAD.state" > $W/test_state_load.txt
rm -f $W/test_warp.txt $W/test_place.txt $W/test_gun.txt; [ -n "${WARP:-}" ] && printf "$WARP" > $W/test_warp.txt; [ -n "${PLACE:-}" ] && printf "$PLACE" > $W/test_place.txt
# GUN="seconds:state" (hex) gives Conker that weapon state then (testing.cpp's CONKER_TEST_GUN; shotgun 3B).
[ -n "${GUN:-}" ] && printf "$GUN" > $W/test_gun.txt
# PHOTO="seconds ..." opens/closes Photo Mode then, PHOTO_SHOT="seconds ..." saves pictures (photo_mode.cpp).
rm -f $W/test_photo.txt $W/test_photo_shot.txt; [ -n "${PHOTO:-}" ] && printf "$PHOTO" > $W/test_photo.txt; [ -n "${PHOTO_SHOT:-}" ] && printf "$PHOTO_SHOT" > $W/test_photo_shot.txt
# PHOTO_VIEW="yaw pitch distance": Photo Mode starts circling Conker from there.
rm -f $W/test_photo_view.txt; [ -n "${PHOTO_VIEW:-}" ] && printf "$PHOTO_VIEW" > $W/test_photo_view.txt
printf "${INPUT:-50:1000,54:1000}${EXTRA_INPUT:+,$EXTRA_INPUT}" > $W/input_script.txt
# With SNAP_ROOM the times are that room's timer (60 a second), not game seconds: run long
# enough to reach the room (about 60 s in) and the last one, not that many seconds.
if [ -n "${SNAP_ROOM:-}" ] && [ -z "${RUN:-}" ]; then
  last=$(echo $secs | tr ' ' '\n' | sort -n | tail -1); export RUN=$(( 75 + ${last%.*} / 60 ))
fi
SPEED=${SPEED:-4} "$(dirname "$0")/snaphost.sh" $secs >/dev/null 2>&1
# Photo Mode pictures from this run go with the run's pictures (the player's own stay).
[ -d $CFG/Photos ] && find $CFG/Photos -maxdepth 1 -name "*.png" -newer $out/.start -exec mv {} $out/ \;
cp $W/snaps/t*.png $W/snaps/sound.wav $W/snaps/*.txt $out/ 2>/dev/null; mv $W/snaps/t*.rdram $out/ 2>/dev/null; tr -d '\0\r' < $W/host_run.log > $out/host_run.log
echo "$name: $(ls $out | tr '\n' ' ')"
