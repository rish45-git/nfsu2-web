@echo off
rem NFSU2 online: host a game for your friends, or join a friend's.  (win/online/friends in the repository)
title NFSU2 online
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0online\Play-Online.ps1" %*
if errorlevel 1 pause
