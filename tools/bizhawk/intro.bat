@echo off
cd /d C:\ConkerRecompWin\bizhawk
if not exist shots_intro mkdir shots_intro
del /q shots_intro\*.* 2>nul
C:\ConkerRecompWin\bizhawk\EmuHawk.exe --lua=C:\ConkerRecompWin\bizhawk\intro.lua C:\ConkerRecompWin\bizhawk\conker.z64
