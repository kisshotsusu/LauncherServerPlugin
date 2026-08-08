@echo off
chcp 65001 >nul
title Import HotPatcher Update Packages
cd /d "%~dp0"

echo Importing HotPatcher outputs (paths read from Server\config.json) ...
python scripts\import_hotpatcher.py

echo.
if errorlevel 1 (
  echo [ERROR] Import failed.
) else (
  echo Import finished. Client will find the new version after CheckForUpdates.
)
pause
