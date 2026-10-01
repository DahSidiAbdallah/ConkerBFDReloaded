@echo off
cd /d C:\ConkerRecompWin\bizhawk
del /q shots_cmp\*.* 2>nul
C:\ConkerRecompWin\bizhawk\EmuHawk.exe --lua=C:\ConkerRecompWin\bizhawk\compare.lua C:\ConkerRecompWin\bizhawk\conker.z64
