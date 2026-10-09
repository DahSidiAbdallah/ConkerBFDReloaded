@echo off
rem Runs our program straight into the game for %1 seconds of game time (default 40),
rem log in host_run.log. Optional: %2 = speed (fast-forward, e.g. 4), %3 = game seconds
rem to screenshot at, comma-separated (snap.ps1 -Log takes them).
set SECS=%~1
if "%SECS%"=="" set SECS=40
set CONKER_NO_CONTROLLER=1
set CONKER_PROBE=1
if exist C:\ConkerRecompWin\log_window.txt set /p CONKER_LOG_WINDOW=<C:\ConkerRecompWin\log_window.txt
if exist C:\ConkerRecompWin\fblog.flag set CONKER_RT64_FBLOG=1
if exist C:\ConkerRecompWin\texdump.flag set RT64_DUMP_TEXTURES=C:\ConkerRecompWin\texdump
if exist C:\ConkerRecompWin\rdramdump.flag set CONKER_RDRAM_DUMP=1
if exist C:\ConkerRecompWin\test_crosshair.flag set CONKER_TEST_CROSSHAIR=1
if exist C:\ConkerRecompWin\cam_trace.flag set CONKER_CAM_TRACE=1
if exist C:\ConkerRecompWin\cam_nozoom.flag set CONKER_CAM_NOZOOM=1
if exist C:\ConkerRecompWin\grab_debug.flag set CONKER_GRAB_DEBUG=1
if exist C:\ConkerRecompWin\swim_debug.flag set CONKER_SWIM_DEBUG=1
if exist C:\ConkerRecompWin\prompt_debug.flag set CONKER_PROMPT_DEBUG=1
if exist C:\ConkerRecompWin\track.flag set CONKER_TEST_TRACK=1
if exist C:\ConkerRecompWin\prompt_pictures.flag set CONKER_PROMPT_PICTURES=C:\ConkerRecompWin\snaps\prompt_pictures
if exist C:\ConkerRecompWin\test_warp.txt set /p CONKER_TEST_WARP=<C:\ConkerRecompWin\test_warp.txt
if exist C:\ConkerRecompWin\test_place.txt set /p CONKER_TEST_PLACE=<C:\ConkerRecompWin\test_place.txt
if exist C:\ConkerRecompWin\test_text.txt set /p CONKER_TEST_TEXT=<C:\ConkerRecompWin\test_text.txt
if exist C:\ConkerRecompWin\test_camera_flags.txt set /p CONKER_TEST_CAMERA_FLAGS=<C:\ConkerRecompWin\test_camera_flags.txt
if exist C:\ConkerRecompWin\test_sink.txt set /p CONKER_TEST_SINK=<C:\ConkerRecompWin\test_sink.txt
if exist C:\ConkerRecompWin\test_device.txt set /p CONKER_TEST_DEVICE=<C:\ConkerRecompWin\test_device.txt
if exist C:\ConkerRecompWin\test_poke.txt set /p CONKER_TEST_POKE=<C:\ConkerRecompWin\test_poke.txt
if exist C:\ConkerRecompWin\vtxlight.flag set RT64_CBFD_VTXLIGHT_LOG=C:\ConkerRecompWin\snaps\vtxlight.txt
if exist C:\ConkerRecompWin\cull43.flag set CONKER_TEST_43_CULLING=1
if exist C:\ConkerRecompWin\old_lightpos.flag set RT64_CBFD_OLD_LIGHTPOS=1
if exist C:\ConkerRecompWin\dist_scale.txt set /p RT64_CBFD_DIST_SCALE=<C:\ConkerRecompWin\dist_scale.txt
if exist C:\ConkerRecompWin\ucode_log.flag set RT64_UCODE_LOG=1
if exist C:\ConkerRecompWin\light_log.flag set RT64_CBFD_LIGHT_LOG=1
if exist C:\ConkerRecompWin\point_dir.flag set RT64_CBFD_POINT_DIR=1
if exist C:\ConkerRecompWin\basic_dir.flag set RT64_CBFD_BASIC_DIR=1
if exist C:\ConkerRecompWin\light_off.flag set RT64_CBFD_LIGHT_OFF=1
if exist C:\ConkerRecompWin\point_scale.txt set /p RT64_CBFD_POINT_SCALE=<C:\ConkerRecompWin\point_scale.txt
if exist C:\ConkerRecompWin\no_texgen.flag set RT64_CBFD_NO_TEXGEN=1
if exist C:\ConkerRecompWin\texgen_log.flag set RT64_CBFD_TEXGEN_LOG=1
if exist C:\ConkerRecompWin\snap_room.txt set /p CONKER_SNAP_ROOM=<C:\ConkerRecompWin\snap_room.txt
if exist C:\ConkerRecompWin\probe_voices.flag set CONKER_PROBE_VOICES=1
if exist C:\ConkerRecompWin\probe_pause.flag set CONKER_PROBE_PAUSE=1
if exist C:\ConkerRecompWin\present_log.flag set RT64_PRESENT_LOG=C:\ConkerRecompWin\snaps\presents.txt
if exist C:\ConkerRecompWin\record.flag set CONKER_RECORD_WAV=C:\ConkerRecompWin\snaps\sound.wav
if not "%~2"=="" set CONKER_SPEED=%~2
if not "%~3"=="" set CONKER_SNAP_AT=%~3
if exist C:\ConkerRecompWin\skip_intro.flag set CONKER_SKIP_INTRO=1
if exist C:\ConkerRecompWin\input_script.txt set /p CONKER_INPUT_SCRIPT=<C:\ConkerRecompWin\input_script.txt
"C:\ConkerRecompWin\host\build\ConkerBFDReloaded.exe" --seconds %SECS% > C:\ConkerRecompWin\host_run.log 2>&1
echo EXITCODE=%errorlevel%
