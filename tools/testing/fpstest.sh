#!/usr/bin/env bash
# NOVIDEO=1: no screen recording (only the frame log).
# Frame-rate check: ./tools/testing/fpstest.sh NAME "graphics:rr_option=Manual graphics:rr_manual_value=120"
# Skip Intro -> first level; from game second 60 Conker runs about and the camera turns
# (stick + C buttons) at real speed, while ffmpeg records the game window at the
# display's rate (60) for 12 s. (PresentMon would need administrator rights.)
# Result: C:\ConkerRecompWin\snaps\ab\NAME\capture.mkv (+ the usual abrun.sh files).
name=$1; changes=$2
W=/mnt/c/ConkerRecompWin
rm -f $W/snaps/capture_$name.mp4 $W/snaps/presents.txt
touch $W/present_log.flag
rm -f $W/host_run.log
(
  [ -n "${NOVIDEO:-}" ] && exit 0
  for i in $(seq 1 600); do
    { tr -d '\0' < $W/host_run.log; } 2>/dev/null | grep -q "\[input\] 54" && break; sleep 0.2
  done
  sleep 5
  rm -f $W/snaps/rect.txt
  timeout 30 powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\ConkerRecompWin\topmost.ps1' -Seconds 15 -RectFile 'C:\ConkerRecompWin\snaps\rect.txt' >/dev/null 2>&1 &
  for i in $(seq 1 50); do [ -s $W/snaps/rect.txt ] && break; sleep 0.1; done
  read x y w h < <(tr -d '\r' < $W/snaps/rect.txt)
  echo "capturing $x $y $w $h"
  timeout 40 $W/tools/ffmpeg/bin/ffmpeg.exe -hide_banner -loglevel error -y -f gdigrab -framerate 60 -offset_x $x -offset_y $y -video_size ${w}x${h} -draw_mouse 0 -i desktop -t 12 -vf "scale=960:-2,format=yuv420p" -c:v libx264 -preset veryfast -crf 16 "C:\\ConkerRecompWin\\snaps\\capture_$name.mp4"
) &
watcher=$!
EXTRA_INPUT="60:x=1.0:3,63:y=1.0:3,66:0001:2,68:x=-1.0:2,70:0002:2" SPEED=1 "$(dirname "$0")/abrun.sh" "$name" "$changes" 76
wait $watcher
rm -f $W/present_log.flag
mv $W/snaps/capture_$name.mp4 $W/snaps/ab/$name/capture.mp4 2>/dev/null || echo "no video"
mv $W/snaps/presents.txt $W/snaps/ab/$name/presents.txt 2>/dev/null || echo "no present log"
"$(dirname "$0")/../../.venv/bin/python" "$(dirname "$0")/presentstats.py" $W/snaps/ab/$name/presents.txt
