@echo off
cd /d "%~dp0"
title Import HotPatcher Update Packages

set "RUN=dist\CloudUpdateServer.exe"
if not exist "%RUN%" set "RUN=python run_server.py"

echo Importing HotPatcher output (paths are read from config.json) ...
echo.
%RUN% --config config.json import-hotpatcher

echo.
if errorlevel 1 (
  echo [ERROR] Import failed.
) else (
  echo Import finished. Clients will see the new version after CheckForUpdates.
)
echo.
pause
