@echo off
rem Fetches clean copies of the runtime, RT64 and RecompFrontend at the versions
rem CBFD-Recompiled (sciaschi, MIT) uses, then applies their patches. Safe to re-run.
setlocal
set "D=C:\ConkerRecompWin\deps"
set "P=C:\ConkerRecompWin\patches"
if not exist "%D%" mkdir "%D%"
call :get N64ModernRuntime https://github.com/N64Recomp/N64ModernRuntime.git cdf5abbd5026fef5c364c676e4667c45e42b6863 || exit /b 1
call :get rt64 https://github.com/rt64/rt64.git 43373749dac9bbc1b653e6a02aed40a9e1783bed || exit /b 1
call :get RecompFrontend https://github.com/N64Recomp/RecompFrontend.git b1a1477c6556aeb7ed45defbfb5924f721efebc1 || exit /b 1
call :patch "%D%\N64ModernRuntime" "%P%\n64modernruntime.patch" || exit /b 1
call :patch "%D%\rt64" "%P%\rt64.patch" || exit /b 1
rem Ours: fast-forward (Skip Intro, test runs) and leaving frames undrawn.
call :patch "%D%\N64ModernRuntime" "%P%\n64modernruntime_conker.patch" || exit /b 1
rem Ours: Conker's reflections (texture generation on the CPU) and the RT64_PRESENT_LOG frame-timing log.
call :patch "%D%\rt64" "%P%\rt64_conker.patch" || exit /b 1
call :patch "%D%\RecompFrontend\recompui\lib\RmlUi" "%P%\rmlui.patch" || exit /b 1
rem Ours: lets the game add RT64 settings the Graphics tab doesn't have.
call :patch "%D%\RecompFrontend" "%P%\recompfrontend_conker.patch" || exit /b 1
rem CBFD-Recompiled V0.1.4+: mouse buttons can be bound.
call :patch "%D%\RecompFrontend" "%P%\recompfrontend.patch" || exit /b 1
rem Our fix: this PC's Windows SDK (10.0.19041) lacks D3D12_HEAP_TYPE_GPU_UPLOAD.
powershell -NoProfile -Command "$f='%D%\rt64\src\contrib\plume\plume_d3d12.cpp'; $t=[IO.File]::ReadAllText($f); $n=$t.Replace('return D3D12_HEAP_TYPE_GPU_UPLOAD;','return D3D12_HEAP_TYPE(5); // D3D12_HEAP_TYPE_GPU_UPLOAD, missing from older Windows SDKs'); [IO.File]::WriteAllText($f,$n)"
echo DEPS OK
exit /b 0

:get
if not exist "%D%\%~1\.git" git clone -q "%~2" "%D%\%~1" || exit /b 1
git -C "%D%\%~1" fetch -q origin %~3 2>nul
git -C "%D%\%~1" checkout -q %~3 || exit /b 1
git -C "%D%\%~1" submodule update --init --recursive -q || exit /b 1
echo   got %~1 at %~3
exit /b 0

:patch
git -C "%~1" apply --reverse --check "%~2" >nul 2>&1 && (echo   already patched %~1& exit /b 0)
git -C "%~1" apply "%~2" || (echo Error: couldn't apply %~2 & exit /b 1)
echo   patched %~1
exit /b 0
