@echo off
rem Run by librespot --onevent. Line 1 = track name, following lines = artists (librespot joins them with newlines).
rem The file is written by `cmd /u` (UTF-16LE): Wine's cmd has no UTF-8 code page (chcp 65001 fails there).
if not defined NAME exit /b
if not "%~1"=="write" (
  cmd /u /c ""%~f0" write"
  move /y nowplaying.tmp nowplaying.txt >nul
  exit /b
)
setlocal EnableDelayedExpansion
(
  echo(!NAME!
  echo(!ARTISTS!
) > nowplaying.tmp
