#!/usr/bin/env bash
# Run our program straight into the game and screenshot it at the given game seconds.
#   ./tools/testing/snaphost.sh [sec ...]     (default 6 10 14 20)
#   SPEED=4 ./tools/testing/snaphost.sh ...   fast-forward (4x; sound muted)
#   RUN=70 ...                                how long to run (game seconds; default last+3)
# The game holds still at each moment until its screenshot is taken, so pictures land
# on the same game moment at any speed. Contact sheet: C:\ConkerRecompWin\snaps\sheet.png
secs="${*:-6 10 14 20}"
last=$(echo $secs | awk '{printf "%d", $NF + 0.999}')
run=${RUN:-$((last+3))}
speed=${SPEED:-1}
list=$(echo $secs | tr ' ' ',')
taskkill.exe /F /IM ConkerBFDReloaded.exe >/dev/null 2>&1; sleep 1
rm -f /mnt/c/ConkerRecompWin/host_run.log
(timeout $((run+120)) cmd.exe /c "C:\\ConkerRecompWin\\run_host.bat $run $speed $(echo $secs | tr ' ' '+')" >/tmp/_snaphost.txt 2>&1 &)
timeout $((run/speed+90)) powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File 'C:\ConkerRecompWin\snap.ps1' -Proc ConkerBFDReloaded -AtList $list -Log 'C:\ConkerRecompWin\host_run.log' 2>&1 | tr -d '\r'
for i in $(seq 1 $((run+60))); do tasklist.exe 2>/dev/null | grep -q ConkerBFDReloaded || break; sleep 1; done
taskkill.exe /F /IM ConkerBFDReloaded.exe >/dev/null 2>&1
timeout 60 powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "C:\\ConkerRecompWin\\sheet.ps1" 2>&1 | tr -d "\r"
