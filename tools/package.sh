#!/usr/bin/env bash
# Make the release downloads in C:\ConkerRecompWin\dist from the last builds:
#   ConkerBFDReloaded-v<version>-Windows.zip    (tools/rebuild_all.sh): the exe, SDL2.dll,
#                                               dxcompiler.dll, dxil.dll, assets\, LICENSE, README.txt
#   ConkerBFDReloaded-v<version>-Linux.tar.gz   (tools/build_linux.sh): the program, the shader
#                                               compiler's libraries, assets/, LICENSE, README.txt
#   ConkerBFDReloaded-v<version>-Source.zip / .tar.gz: this folder without what .gitignore keeps out
#                                               (the ROM and anything made from it, downloads, builds)
#   tools/package.sh [LINUX_BUILD_DIR]          (default: build-linux/ here)
# The player adds their own ROM; the program asks for it the first time.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
W=${CONKER_WIN_DIR:-/mnt/c/ConkerRecompWin}
B=$W/host/build
LB=$(realpath -m "${1:-$R/build-linux}")
v() { sed -n "s/^set(CONKER_VERSION_$1 \"\{0,1\}\([^\")]*\)\"\{0,1\})/\1/p" "$R/host/CMakeLists.txt"; }
version="$(v MAJOR).$(v MINOR).$(v PATCH)$(v SUFFIX)"
mkdir -p "$W/dist"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

license() { # the program's license (GPL-3.0, because of N64ModernRuntime), ours, then the others'
  printf 'This program as a whole is distributed under the GNU General Public License, version 3\n'
  printf '(full text at the end of this file), because it includes N64ModernRuntime, which is under\n'
  printf 'that license. Its source code: https://github.com/DahSidiAbdallah/ConkerBFDReloaded\n'
  printf 'The project'"'"'s own code is also available under the MIT License below.\n\n'
  printf -- '----------------------------------------------------------------------\nConker'"'"'s Bad Fur Day: Reloaded (MIT)\n----------------------------------------------------------------------\n\n'
  cat "$R/LICENSE"
  printf '\n\n----------------------------------------------------------------------\nThird-party software\n----------------------------------------------------------------------\n\n'
  printf 'Parts of this program come from CBFD-Recompiled (https://github.com/sciaschi/CBFD-Recompiled):\n\n'
  cat "$R/recomp/THIRD_PARTY_LICENSE"
  printf '\n\nRT64 (https://github.com/rt64/rt64):\n\n'
  tr -d '\r' < "$W/deps/rt64/LICENSE"
  printf '\n\nAlso built with N64Recomp (MIT), N64ModernRuntime (GPL-3.0, below) and RecompFrontend\n(https://github.com/N64Recomp; no license file published), RmlUi (MIT), SDL2 (zlib license)\nand the DirectX Shader Compiler (University of Illinois/NCSA license). The fonts in assets are\nunder the SIL Open Font License (license files next to them).\n\nConker'"'"'s Bad Fur Day is (c) Rare / Microsoft. This project is not affiliated with them.\n'
  printf '\n\n----------------------------------------------------------------------\nGNU General Public License, version 3 (N64ModernRuntime, and this program as a whole)\n----------------------------------------------------------------------\n\n'
  tr -d '\r' < "$W/deps/N64ModernRuntime/COPYING"
}

readme() { # $1: Windows or Linux
  cat <<EOF
Conker's Bad Fur Day: Reloaded  v$version
=============================================

Conker's Bad Fur Day (N64) running natively on PC: the original game, recompiled, with a
Classic mode true to the N64 and a Modern mode (widescreen, smooth frame rate, free camera...).

HOW TO PLAY
EOF
  if [ "$1" = Windows ]; then
    cat <<'EOF'
 1. Run ConkerBFDReloaded.exe.
 2. The first time, choose your own copy of the game: the US ROM
    (Conker's Bad Fur Day (USA).z64). No game data is included.
 3. Pick Classic (as on the N64) or Modern on the Conker tab of the settings.

Your settings and saves are kept in %LOCALAPPDATA%\ConkerBFDReloaded.

Needs Windows 10 or 11 (64-bit) and a graphics card with DirectX 12 or Vulkan.
EOF
  else
    cat <<'EOF'
 1. Run ./ConkerBFDReloaded
 2. The first time, choose your own copy of the game: the US ROM
    (Conker's Bad Fur Day (USA).z64). No game data is included.
    If the file picker doesn't appear, run it once as
    ./ConkerBFDReloaded --rom /path/to/conker.z64   (it's remembered after that).
 3. Pick Classic (as on the N64) or Modern on the Conker tab of the settings.

Your settings and saves are kept in ~/.config/ConkerBFDReloaded.

Needs a 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Fedora 36, Debian 12 or
newer), a Vulkan graphics driver, and SDL2, GTK 3 and FreeType.

The Linux version is EXPERIMENTAL: it has only been tested inside WSL with software
graphics so far. Please report how it runs for you.
EOF
  fi
  cat <<'EOF'

Made by dahmedvall95.

Credits: CBFD-Recompiled (Sean Ciaschi), the mkst/conker decompilation,
N64Recomp / N64ModernRuntime / RecompFrontend (Wiseguy and contributors), RT64 (Dario
and contributors), GameBeast92 (4K texture pack artwork, for the optional HD Icons mod).
See LICENSE.
EOF
}

# Windows.
name=ConkerBFDReloaded-v$version-Windows
D=$W/dist/$name
rm -rf "$D" "$W/dist/$name.zip"; mkdir -p "$D"
cp "$B/ConkerBFDReloaded.exe" "$B/SDL2.dll" "$B/dxcompiler.dll" "$B/dxil.dll" "$D/"
cp -r "$R/host/assets" "$D/assets"
license | sed 's/$/\r/' > "$D/LICENSE"
readme Windows | sed 's/$/\r/' > "$D/README.txt"
(cd "$W/dist" && powershell.exe -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$name.zip' -Force" >/dev/null)
echo "$W/dist/$name.zip"

# Linux.
if [ -x "$LB/ConkerBFDReloaded" ]; then
  name=ConkerBFDReloaded-v$version-Linux
  D=$T/$name
  mkdir -p "$D"
  cp "$LB/ConkerBFDReloaded" "$LB/libdxcompiler.so" "$LB/libdxil.so" "$D/"
  strip "$D/ConkerBFDReloaded"
  cp -r "$R/host/assets" "$D/assets"
  license > "$D/LICENSE"
  readme Linux > "$D/README.txt"
  tar -C "$T" -czf "$W/dist/$name.tar.gz" "$name"
  echo "$W/dist/$name.tar.gz"
else
  echo "(no Linux build in $LB: skipped the Linux download)"
fi

# Source code: the files git would track (everything .gitignore doesn't keep out).
name=ConkerBFDReloaded-v$version-Source
D=$T/$name
mkdir -p "$D"
git -C "$R" rev-parse --git-dir >/dev/null 2>&1 || { echo "not a git repository: run git init in $R first"; exit 1; }
(cd "$R" && { git ls-files; git ls-files --others --exclude-standard; } | sort -u | while read -r f; do
  [ -f "$f" ] && mkdir -p "$D/$(dirname "$f")" && cp -p "$f" "$D/$f"; done)
tar -C "$T" -czf "$W/dist/$name.tar.gz" "$name"
(cd "$T" && rm -f "$W/dist/$name.zip" && python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[2])" "$W/dist/$name" "$name")
echo "$W/dist/$name.tar.gz"; echo "$W/dist/$name.zip"
ls -la "$W/dist" | grep "v$version"
