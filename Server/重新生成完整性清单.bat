@echo off
cd /d "%~dp0"
title Regenerate Integrity Manifest

set "RUN=dist\CloudUpdateServer.exe"
if not exist "%RUN%" set "RUN=python run_server.py"

echo Scanning the package directory and regenerating the manifest ...
echo.
%RUN% --config config.json gen-manifest --platform Windows

echo.
if errorlevel 1 (
  echo [ERROR] Manifest generation failed.
) else (
  echo Manifest regenerated.
)
echo.
pause
