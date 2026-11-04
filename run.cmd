@echo off
rem Double-click to start the recompiled game. It writes nba2k18.log next to this file.
cd /d "%~dp0"
if not exist NBA2K18.exe (
  echo NBA2K18.exe not found. Run build.cmd first.
  pause
  exit /b 1
)
NBA2K18.exe "%~dp0private\game"
echo.
echo NBA2K18 exited with code %ERRORLEVEL%. The log is in nba2k18.log. Press any key to close.
pause >nul
