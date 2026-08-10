@echo off
chcp 65001 >nul
title Regenerate Integrity Manifest
cd /d "%~dp0"

set "RUN=python run_server.py"
if exist "dist\CloudUpdateServer.exe" set "RUN=dist\CloudUpdateServer.exe"

echo Scanning package directory and regenerating manifest ...
%RUN% gen-manifest --platform Windows

echo.
if errorlevel 1 (
  echo [ERROR] Manifest generation failed.
) else (
  echo Manifest regenerated.
)
pause
