@echo off
rem Double-click to build NBA2K18.exe (run run-setup.cmd first).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1"
echo.
echo Build finished with exit code %ERRORLEVEL%. Press any key to close.
pause >nul
