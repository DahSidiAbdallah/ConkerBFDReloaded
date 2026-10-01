#!/usr/bin/env bash
# Prepare the game for translation (once, or after the decomp changes):
#  1. the decomp's ELF (conker/conker/build/conker.us.elf, built from mkst/conker with
#     the ROM) -> its code segments recomp/{init,game,debugger}.us.bin and
#     recomp/conker.us.recomp.elf, ready for N64Recomp (recomp/prepare_elf.py);
#  2. the audio microcode -> recomp/out/rsp/audio_ucode.cpp (RSPRecomp, recomp/audio_ucode.toml);
#  3. recomp/out/segments.c (tools/emit_segments.py), after a first translation.
# Then tools/rebuild_all.sh --force. Needs conker.z64 (the US ROM) in the project folder,
# N64Recomp built in N64Recomp/build, and mips-linux-gnu-objcopy.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
E=$R/conker/conker/build/conker.us.elf
cd "$R/recomp"
for s in init game debugger; do mips-linux-gnu-objcopy -O binary --only-section=.$s "$E" $s.us.bin; done
python3 prepare_elf.py "$E" conker.us.recomp.elf "$R/N64Recomp/src/symbol_lists.cpp" \
  --original .init=init.us.bin --original .game=game.us.bin --original .debugger=debugger.us.bin
mkdir -p out/rsp
"$R/N64Recomp/build/RSPRecomp" audio_ucode.toml
if [ -f out/recomp_overlays.inl ]; then
  python3 "$R/tools/emit_segments.py" "$R/recomp" out/segments.c
else
  echo "Run tools/rebuild_all.sh --force, then this script again for out/segments.c."
fi
