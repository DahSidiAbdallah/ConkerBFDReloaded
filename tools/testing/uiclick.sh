#!/usr/bin/env bash
# Clicks through the real settings UI (launcher, no test shortcuts) and screenshots it, with the
# player's settings and save backed up first and always put back.
#   tools/testing/uiclick.sh NAME "x,y;x,y;..." "sec;sec;..." [snap secs, comma-separated] [run secs]
# Positions are in the 1618x947 window (launcher Settings button 1478,753, twice; Conker tab
# 142,122; Classic 128,216; Modern 232,216; "ESC" presses Escape). GAME=1 starts straight into the
# game (Skip Intro) instead of the launcher. Pictures and log in C:\ConkerRecompWin\snaps\ui\NAME.
set -u
name=$1; points=$2; delays=$3; snaps=${4:-}; run=${5:-40}
W=/mnt/c/ConkerRecompWin
CFG=$(ls -d /mnt/c/Users/*/AppData/Local/ConkerBFDReloaded | head -1)
out=$W/snaps/ui/$name; rm -rf "$out"; mkdir -p "$out"
B=$(mktemp -d)
for f in graphics sound conker mods general controls accessibility; do cp "$CFG/$f.json" "$B/" 2>/dev/null; done
cp "$CFG/saves/conker.n64.us.1.0.bin" "$B/save.bin"
restore() {
  mkdir -p "$out/config_after"; cp "$CFG"/*.json "$out/config_after/" 2>/dev/null
  taskkill.exe /F /IM ConkerBFDReloaded.exe >/dev/null 2>&1; sleep 1
  for f in graphics sound conker mods general controls accessibility; do [ -f "$B/$f.json" ] && cp "$B/$f.json" "$CFG/"; done
  cp "$B/save.bin" "$CFG/saves/conker.n64.us.1.0.bin"; rm -rf "$B"
}
trap restore EXIT
trap 'exit 130' INT TERM HUP
taskkill.exe /F /IM ConkerBFDReloaded.exe >/dev/null 2>&1; sleep 1
before=$(wc -c < "$W/host/build/crash.log" 2>/dev/null || echo 0)
args=""; [ -n "${GAME:-}" ] && args="--seconds $((run + 30))"
(cmd.exe /c "set CONKER_SKIP_INTRO=1&& C:\\ConkerRecompWin\\host\\build\\ConkerBFDReloaded.exe $args > C:\\ConkerRecompWin\\snaps\\ui\\$name\\run.log 2>&1" >/dev/null 2>&1 &)
[ -n "$snaps" ] && (timeout $((run + 10)) powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\ConkerRecompWin\snap.ps1' -Proc ConkerBFDReloaded -AtList "$snaps" -Out "C:\\ConkerRecompWin\\snaps\\ui\\$name" >/dev/null 2>&1 &)
timeout $((run + 10)) powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\ConkerRecompWin\click.ps1' -Points "$points" -Delays "$delays" 2>&1 | tr -d '\r' | tail -3
sleep $((run / 4))
if tasklist.exe | grep -qi ConkerBFDReloaded; then echo "still running"; else echo "NOT RUNNING (crashed or quit)"; fi
after=$(wc -c < "$W/host/build/crash.log" 2>/dev/null || echo 0)
[ "$after" -gt "$before" ] && { echo "NEW CRASH:"; tail -c $((after - before)) "$W/host/build/crash.log" | tr -d '\0\r' | head -25; }
ls "$out"
