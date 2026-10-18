@echo off
rem Run by librespot --onevent. Line 1 = track name, following lines = artists (librespot joins them with newlines).
if not defined NAME exit /b
chcp 65001 >nul
setlocal EnableDelayedExpansion
> nowplaying.tmp (
  echo(!NAME!
  echo(!ARTISTS!
)
move /y nowplaying.tmp nowplaying.txt >nul
