@echo off
cd /d C:\ConkerRecompWin\bizhawk
if not exist shots_intro mkdir shots_intro
C:\ConkerRecompWin\bizhawk\EmuHawk.exe --lua=C:\ConkerRecompWin\bizhawk\pub.lua C:\ConkerRecompWin\bizhawk\conker.z64
