@echo off
chcp 65001 >nul
title Import HotPatcher Update Packages
cd /d "%~dp0"

set "RUN=python run_server.py"
if exist "dist\CloudUpdateServer.exe" set "RUN=dist\CloudUpdateServer.exe"

echo Importing HotPatcher outputs (paths read from Server\config.json) ...
%RUN% import-hotpatcher

echo.
if errorlevel 1 (
  echo [ERROR] Import failed.
) else (
  echo Import finished. Client will find the new version after CheckForUpdates.
)
pause
