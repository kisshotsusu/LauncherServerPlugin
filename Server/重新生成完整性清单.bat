@echo off
chcp 65001 >nul
title Regenerate Integrity Manifest
cd /d "%~dp0"

echo Scanning package directory and regenerating manifest ...
python scripts\generate_manifest.py --platform Windows --force

echo.
if errorlevel 1 (
  echo [ERROR] Manifest generation failed.
) else (
  echo Manifest regenerated.
)
pause
