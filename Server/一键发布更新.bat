@echo off
cd /d "%~dp0"
title One-Click Publish Update

set "RUN=dist\CloudUpdateServer.exe"
if not exist "%RUN%" set "RUN=python run_server.py"

echo ============ Step 1/2 : Import HotPatcher packages ============
%RUN% --config config.json import-hotpatcher
if errorlevel 1 (
  echo.
  echo [ERROR] Import failed - aborting.
  echo.
  pause
  exit /b 1
)

echo.
echo ============ Step 2/2 : Regenerate integrity manifest ============
%RUN% --config config.json gen-manifest --platform Windows
if errorlevel 1 (
  echo.
  echo [ERROR] Manifest generation failed.
  echo.
  pause
  exit /b 1
)

echo.
echo ============================================================
echo   Publish finished.
echo   Start the server with Start.bat, then manage everything
echo   in the web console at http://127.0.0.1:8710/
echo ============================================================
echo.
pause
