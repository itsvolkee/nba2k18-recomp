@echo off
rem Double-click to run the setup. Pass a different ISO path with:  run-setup.cmd "D:\path\to\game.iso"
cd /d "%~dp0"
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" -Repo itsvolkee/nba2k18-recomp
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" -Repo itsvolkee/nba2k18-recomp -Iso "%~1"
)
echo.
echo Setup finished with exit code %ERRORLEVEL%. Press any key to close.
pause >nul
