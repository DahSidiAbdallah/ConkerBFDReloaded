@echo off
rem Builds our program: C:\ConkerRecompWin\host -> host\build\ConkerBFDReloaded.exe
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;C:\ConkerRecompWin\tools;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
taskkill /f /im ConkerBFDReloaded.exe >nul 2>&1
if not exist C:\ConkerRecompWin\host\build\build.ninja "C:\Program Files\CMake\bin\cmake.exe" -S C:\ConkerRecompWin\host -B C:\ConkerRecompWin\host\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
"C:\Program Files\CMake\bin\cmake.exe" --build C:\ConkerRecompWin\host\build -- -k 0 || exit /b 1
echo BUILD OK
